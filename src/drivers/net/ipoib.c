/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <byteswap.h>
#include <errno.h>
#include <gpxe/if_arp.h>
#include <gpxe/iobuf.h>
#include <gpxe/netdevice.h>
#include <gpxe/infiniband.h>
#include <gpxe/ipoib.h>

/** @file
 *
 * IP over Infiniband
 */





extern unsigned long hack_ipoib_qkey;
extern struct ib_address_vector hack_ipoib_bcast_av;



/** IPoIB MTU */
#define IPOIB_MTU 2048

/** Number of IPoIB send work queue entries */
#define IPOIB_DATA_NUM_SEND_WQES 4

/** Number of IPoIB receive work queue entries */
#define IPOIB_DATA_NUM_RECV_WQES 8

/** Number of IPoIB completion entries */
#define IPOIB_DATA_NUM_CQES 8

/** An IPoIB queue set */
struct ipoib_queue_set {
	/** Completion queue */
	struct ib_completion_queue *cq;
	/** Queue pair */
	struct ib_queue_pair *qp;
	/** Receive work queue fill level */
	unsigned int recv_fill;
	/** Receive work queue maximum fill level */
	unsigned int recv_max_fill;
};

/** An IPoIB device */
struct ipoib_device {
	/** Network device */
	struct net_device *netdev;
	/** Underlying Infiniband device */
	struct ib_device *ibdev;
	/** Data queue set */
	struct ipoib_queue_set data;
	/** Data queue set */
	struct ipoib_queue_set meta;
};

/****************************************************************************
 *
 * IPoIB link layer
 *
 ****************************************************************************
 */

/** Broadcast IPoIB address */
static struct ipoib_mac ipoib_broadcast = {
	.gid = { { 0xff, 0x12, 0x40, 0x1b, 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff } },
};

/**
 * Transmit IPoIB packet
 *
 * @v iobuf		I/O buffer
 * @v netdev		Network device
 * @v net_protocol	Network-layer protocol
 * @v ll_dest		Link-layer destination address
 *
 * Prepends the IPoIB link-layer header and transmits the packet.
 */
static int ipoib_tx ( struct io_buffer *iobuf, struct net_device *netdev,
		      struct net_protocol *net_protocol,
		      const void *ll_dest ) {
	struct ipoib_hdr *ipoib_hdr =
		iob_push ( iobuf, sizeof ( *ipoib_hdr ) );

	/* Build IPoIB header */
	memcpy ( &ipoib_hdr->pseudo.peer, ll_dest,
		 sizeof ( ipoib_hdr->pseudo.peer ) );
	ipoib_hdr->real.proto = net_protocol->net_proto;
	ipoib_hdr->real.reserved = 0;

	/* Hand off to network device */
	return netdev_tx ( netdev, iobuf );
}

/**
 * Process received IPoIB packet
 *
 * @v iobuf	I/O buffer
 * @v netdev	Network device
 *
 * Strips off the IPoIB link-layer header and passes up to the
 * network-layer protocol.
 */
static int ipoib_rx ( struct io_buffer *iobuf, struct net_device *netdev ) {
	struct ipoib_hdr *ipoib_hdr = iobuf->data;

	/* Sanity check */
	if ( iob_len ( iobuf ) < sizeof ( *ipoib_hdr ) ) {
		DBG ( "IPoIB packet too short (%d bytes)\n",
		      iob_len ( iobuf ) );
		free_iob ( iobuf );
		return -EINVAL;
	}

	/* Strip off IPoIB header */
	iob_pull ( iobuf, sizeof ( *ipoib_hdr ) );

	/* Hand off to network-layer protocol */
	return net_rx ( iobuf, netdev, ipoib_hdr->real.proto,
			&ipoib_hdr->pseudo.peer );
}

/**
 * Transcribe IPoIB address
 *
 * @v ll_addr	Link-layer address
 * @ret string	Link-layer address in human-readable format
 */
const char * ipoib_ntoa ( const void *ll_addr ) {
	static char buf[61];
	const uint8_t *ipoib_addr = ll_addr;
	unsigned int i;
	char *p = buf;

	for ( i = 0 ; i < IPOIB_ALEN ; i++ ) {
		p += sprintf ( p, ":%02x", ipoib_addr[i] );
	}
	return ( buf + 1 );
}

/** IPoIB protocol */
struct ll_protocol ipoib_protocol __ll_protocol = {
	.name		= "IPoIB",
	.ll_proto	= htons ( ARPHRD_INFINIBAND ),
	.ll_addr_len	= IPOIB_ALEN,
	.ll_header_len	= IPOIB_HLEN,
	.ll_broadcast	= ( uint8_t * ) &ipoib_broadcast,
	.tx		= ipoib_tx,
	.rx		= ipoib_rx,
	.ntoa		= ipoib_ntoa,
};

/****************************************************************************
 *
 * IPoIB network device
 *
 ****************************************************************************
 */

/**
 * Destroy queue set
 *
 * @v ipoib		IPoIB device
 * @v qset		Queue set
 */
static void ipoib_destroy_qset ( struct ipoib_device *ipoib,
				 struct ipoib_queue_set *qset ) {
	struct ib_device *ibdev = ipoib->ibdev;

	if ( qset->qp )
		ib_destroy_qp ( ibdev, qset->qp );
	if ( qset->cq )
		ib_destroy_cq ( ibdev, qset->cq );
	memset ( qset, 0, sizeof ( *qset ) );
}

/**
 * Create queue set
 *
 * @v ipoib		IPoIB device
 * @v qset		Queue set
 * @ret rc		Return status code
 */
static int ipoib_create_qset ( struct ipoib_device *ipoib,
			       struct ipoib_queue_set *qset,
			       unsigned int num_cqes,
			       unsigned int num_send_wqes,
			       unsigned int num_recv_wqes,
			       unsigned long qkey ) {
	struct ib_device *ibdev = ipoib->ibdev;
	int rc;

	/* Store queue parameters */
	qset->recv_max_fill = num_recv_wqes;

	/* Allocate completion queue */
	qset->cq = ib_create_cq ( ibdev, num_cqes );
	if ( ! qset->cq ) {
		DBGC ( ipoib, "IPoIB %p could not allocate completion queue\n",
		       ipoib );
		rc = -ENOMEM;
		goto err;
	}

	/* Allocate queue pair */
	qset->qp = ib_create_qp ( ibdev, num_send_wqes, qset->cq,
				  num_recv_wqes, qset->cq, qkey );
	if ( ! qset->qp ) {
		DBGC ( ipoib, "IPoIB %p could not allocate queue pair\n",
		       ipoib );
		rc = -ENOMEM;
		goto err;
	}
	qset->qp->owner_priv = ipoib->netdev;

	return 0;

 err:
	ipoib_destroy_qset ( ipoib, qset );
	return rc;
}

/**
 * Transmit packet via IPoIB network device
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int ipoib_transmit ( struct net_device *netdev,
			    struct io_buffer *iobuf ) {
	struct ipoib_device *ipoib = netdev->priv;
	struct ib_device *ibdev = ipoib->ibdev;
	struct ipoib_pseudo_hdr *ipoib_pshdr = iobuf->data;

	if ( iob_len ( iobuf ) < sizeof ( *ipoib_pshdr ) ) {
		DBGC ( ipoib, "IPoIB %p buffer too short\n", ipoib );
		return -EINVAL;
	}

	iob_pull ( iobuf, ( sizeof ( *ipoib_pshdr ) ) );
	return ib_post_send ( ibdev, ipoib->data.qp,
			      &hack_ipoib_bcast_av, iobuf );
}

/**
 * Handle IPoIB send completion
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v completion	Completion
 * @v iobuf		I/O buffer
 */
static void ipoib_data_complete_send ( struct ib_device *ibdev __unused,
				       struct ib_queue_pair *qp,
				       struct ib_completion *completion,
				       struct io_buffer *iobuf ) {
	struct net_device *netdev = qp->owner_priv;

	netdev_tx_complete_err ( netdev, iobuf,
				 ( completion->syndrome ? -EIO : 0 ) );
}

/**
 * Handle IPoIB receive completion
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v completion	Completion
 * @v iobuf		I/O buffer
 */
static void ipoib_data_complete_recv ( struct ib_device *ibdev __unused,
				       struct ib_queue_pair *qp,
				       struct ib_completion *completion,
				       struct io_buffer *iobuf ) {
	struct net_device *netdev = qp->owner_priv;
	struct ipoib_device *ipoib = netdev->priv;
	struct ib_global_route_header *grh = iobuf->data;
	struct ipoib_pseudo_hdr *ipoib_pshdr;

	if ( completion->syndrome ) {
		netdev_rx_err ( netdev, iobuf, -EIO );
	} else {
		iob_put ( iobuf, completion->len );
		iob_pull ( iobuf, ( sizeof ( *grh ) -
				    sizeof ( *ipoib_pshdr ) ) );
		/* FIXME: fill in a MAC address for the sake of AoE! */
		netdev_rx ( netdev, iobuf );
	}

	ipoib->data.recv_fill--;
}

/**
 * Refill IPoIB receive ring
 *
 * @v ipoib		IPoIB device
 */
static void ipoib_refill_recv ( struct ipoib_device *ipoib,
				struct ipoib_queue_set *qset ) {
	struct ib_device *ibdev = ipoib->ibdev;
	struct io_buffer *iobuf;
	int rc;

	while ( qset->recv_fill < qset->recv_max_fill ) {
		iobuf = alloc_iob ( IPOIB_MTU );
		if ( ! iobuf )
			break;
		if ( ( rc = ib_post_recv ( ibdev, qset->qp, iobuf ) ) != 0 ) {
			free_iob ( iobuf );
			break;
		}
		qset->recv_fill++;
	}
}

/**
 * Poll IPoIB network device
 *
 * @v netdev		Network device
 */
static void ipoib_poll ( struct net_device *netdev ) {
	struct ipoib_device *ipoib = netdev->priv;
	struct ib_device *ibdev = ipoib->ibdev;

	ib_poll_cq ( ibdev, ipoib->data.cq, ipoib_data_complete_send,
		     ipoib_data_complete_recv );
	ipoib_refill_recv ( ipoib, &ipoib->data );
}

/**
 * Enable/disable interrupts on IPoIB network device
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
static void ipoib_irq ( struct net_device *netdev __unused,
			int enable __unused ) {
	/* No implementation */
}

/**
 * Open IPoIB network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int ipoib_open ( struct net_device *netdev ) {
	struct ipoib_device *ipoib = netdev->priv;
	struct ib_device *ibdev = ipoib->ibdev;
	int rc;

	/* Attach to broadcast multicast GID */
	if ( ( rc = ib_mcast_attach ( ibdev, ipoib->data.qp,
				      &ibdev->broadcast_gid ) ) != 0 ) {
		DBG ( "Could not attach to broadcast GID: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Fill receive ring */
	ipoib_refill_recv ( ipoib, &ipoib->data );

	return 0;
}

/**
 * Close IPoIB network device
 *
 * @v netdev		Network device
 */
static void ipoib_close ( struct net_device *netdev ) {
	struct ipoib_device *ipoib = netdev->priv;
	struct ib_device *ibdev = ipoib->ibdev;

	/* Detach from broadcast multicast GID */
	ib_mcast_detach ( ibdev, ipoib->data.qp, &ipoib_broadcast.gid );

	/* FIXME: should probably flush the receive ring */
}

/** IPoIB network device operations */
static struct net_device_operations ipoib_operations = {
	.open		= ipoib_open,
	.close		= ipoib_close,
	.transmit	= ipoib_transmit,
	.poll		= ipoib_poll,
	.irq		= ipoib_irq,
};

/**
 * Probe IPoIB device
 *
 * @v ibdev		Infiniband device
 * @ret rc		Return status code
 */
int ipoib_probe ( struct ib_device *ibdev ) {
	struct net_device *netdev;
	struct ipoib_device *ipoib;
	struct ipoib_mac *mac;
	int rc;

	/* Allocate network device */
	netdev = alloc_ipoibdev ( sizeof ( *ipoib ) );
	if ( ! netdev )
		return -ENOMEM;
	netdev_init ( netdev, &ipoib_operations );
	ipoib = netdev->priv;
	ib_set_ownerdata ( ibdev, netdev );
	netdev->dev = ibdev->dev;
	memset ( ipoib, 0, sizeof ( *ipoib ) );
	ipoib->netdev = netdev;
	ipoib->ibdev = ibdev;

	/* Allocate data queue set */
	if ( ( rc = ipoib_create_qset ( ipoib, &ipoib->data,
					IPOIB_DATA_NUM_CQES,
					IPOIB_DATA_NUM_SEND_WQES,
					IPOIB_DATA_NUM_RECV_WQES,
					hack_ipoib_qkey ) ) != 0 ) {
		DBGC ( ipoib, "IPoIB %p could not allocate data QP: %s\n",
		       ipoib, strerror ( rc ) );
		goto err_create_data_qset;
	}

	/* Construct MAC address */
	mac = ( ( struct ipoib_mac * ) netdev->ll_addr );
	mac->qpn = htonl ( ipoib->data.qp->qpn );
	memcpy ( &mac->gid, &ibdev->port_gid, sizeof ( mac->gid ) );

	/* Register network device */
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register_netdev;

	return 0;

 err_register_netdev:
	ipoib_destroy_qset ( ipoib, &ipoib->data );
 err_create_data_qset:
	netdev_nullify ( netdev );
	netdev_put ( netdev );
	return rc;
}

/**
 * Remove IPoIB device
 *
 * @v ibdev		Infiniband device
 */
void ipoib_remove ( struct ib_device *ibdev ) {
	struct net_device *netdev = ib_get_ownerdata ( ibdev );

	unregister_netdev ( netdev );
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}
