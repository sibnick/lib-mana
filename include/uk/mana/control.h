/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_CONTROL_H
#define UK_MANA_CONTROL_H

#include <uk/mana/bootstrap.h>

#define MANA_CONTROL_INVALID_HANDLE UINT64_MAX

enum mana_control_queue_type {
	MANA_CONTROL_TX = 1,
	MANA_CONTROL_RX = 2
};

/* Regions must belong to the registered PD. Create consumes both regions on
 * success. Retain their DMA memory until destroy succeeds. After an uncertain
 * create, retain both region handles and memory without reuse or destruction.
 * The caller creates and services the parent EQ outside this control API.
 */
struct mana_control_queue_spec {
	uint64_t wq_region, cq_region;
	uint32_t wq_size, cq_size;
	uint32_t parent_eq, moderation_context;
};

struct mana_control;
struct mana_control_queue {
	struct mana_control *control;
	struct mana_control_queue_spec spec;
	enum mana_control_queue_type type;
	uint64_t handle;
	uint32_t wq_id, cq_id;
	unsigned int live, quarantined, fence_pending, fenced;
};

/* Use fresh, zeroed storage. Serialize all calls with bootstrap operations.
 * Keep control and queue storage alive until stop/destroy succeeds.
 * This API supports one port and one TX/RX queue pair. It owns no DMA memory.
 * A failed start can own registration. Call stop unless quarantined.
 */
struct mana_control {
	struct mana_bootstrap *bootstrap;
	struct mana_control_queue *tx, *rx;
	uint64_t capabilities[4], vport;
	uint32_t pd_id, mkey, doorbell_id;
	uint32_t max_eqs, max_sq, max_rq, indirection_entries;
	uint32_t link_speed_mbps, qos_speed_mbps;
	uint16_t max_ports, frame_mtu, mtu, tx_vport_offset;
	unsigned char mac[6];
	unsigned int registered, configured, ready, quarantined, data_owned;
	/* bm_hostmode and short_form_allowed record response bytes as the
	 * pin stores them. short_form_allowed is normalized to 0 or 1.
	 */
	unsigned int rx_enabled, short_form_allowed, bm_hostmode;
	int link_query_error;
};

int mana_control_start(struct mana_control *c, struct mana_bootstrap *b,
		       uint32_t port_index);
int mana_control_stop(struct mana_control *c);
int mana_control_poll(struct mana_control *c);
int mana_control_query_link(struct mana_control *c);
int mana_control_set_mtu(struct mana_control *c, uint16_t mtu);
int mana_control_create_queue(struct mana_control *c,
			      struct mana_control_queue *q,
			      enum mana_control_queue_type type,
			      const struct mana_control_queue_spec *spec);
int mana_control_destroy_queue(struct mana_control_queue *q);
int mana_control_set_rx(struct mana_control *c, unsigned int enable);
/* Fence acknowledgment does not mean DMA has stopped. The datapath calls
 * fence_complete only for a validated CQE_RX_OBJECT_FENCE for this object.
 * wait_fence calls the caller's CQ poll at most 10000 times with 1 ms delays.
 * The CQ poll returns zero or a negative errno and must not block.
 */
int mana_control_fence_rx(struct mana_control_queue *q);
int mana_control_fence_complete(struct mana_control_queue *q);
int mana_control_wait_fence(struct mana_control_queue *q,
			    int (*poll)(void *), void *ctx);

#endif
