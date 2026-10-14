/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_NETDEV_H
#define UK_MANA_NETDEV_H

#include <uk/netdev.h>
#include <uk/mana/tx.h>
#include <uk/mana/rx.h>

#define MANA_NETDEV_DESCRIPTORS 32U

struct mana_netdev;

struct uk_netdev_tx_queue {
	struct mana_netdev *owner;
	uint16_t descriptors;
};

struct uk_netdev_rx_queue {
	struct mana_netdev *owner;
	uint16_t descriptors;
	uk_netdev_alloc_rxpkts alloc;
	void *alloc_ctx;
};

/* Retain this storage and the bootstrap/platform allocator after registration.
 * The pinned core has no unregister API. Serialize all calls outside IRQ
 * context. RX copies into caller netbufs and releases transferred DMA before
 * return. Caller netbuf allocators must remain live until their last release.
 */
struct mana_netdev {
	struct uk_netdev dev;
	struct mana_bootstrap *bootstrap;
	struct uk_alloc *alloc;
	struct uk_hwaddr mac;
	uint16_t mtu, max_mtu;
	struct mana_control control;
	struct mana_data data;
	struct mana_tx tx;
	struct mana_rx rx;
	struct mana_tx_record records[MANA_NETDEV_DESCRIPTORS];
	struct mana_rx_slot slots[MANA_NETDEV_DESCRIPTORS];
	struct uk_netdev_tx_queue txq;
	struct uk_netdev_rx_queue rxq;
	unsigned int registered, configured, running, stopping, shutdown;
};

/* Supply fresh, zeroed storage and a ready bootstrap. Return the netdev ID.
 * Failed registration cleans up proven ownership. Failed cleanup retains
 * storage for shutdown retry. Never register a DMA-blocked bootstrap.
 */
int mana_netdev_register(struct mana_netdev *n, struct mana_bootstrap *b,
			 struct uk_alloc *alloc);
/* The pinned ops table has no stop. Successful stop restores CONFIGURED
 * state for uk_netdev_start. It keeps queue configuration and control.
 * Failed stop retains resources and blocks restart until stop succeeds.
 */
int mana_netdev_stop(struct mana_netdev *n);
/* Stop queues, then release control before the caller stops bootstrap.
 * Registration stays live but inert. This is a terminal operation.
 */
int mana_netdev_shutdown(struct mana_netdev *n);
/* Service accepted TX cookies even when no new packets arrive. */
int mana_netdev_poll(struct mana_netdev *n);

#endif
