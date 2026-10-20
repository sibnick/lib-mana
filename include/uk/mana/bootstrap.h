/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_BOOTSTRAP_H
#define UK_MANA_BOOTSTRAP_H

#include <uk/mana/platform.h>

#define MANA_HWC_PAGE_COUNT 6U
#define MANA_HWC_EQ 0U
#define MANA_HWC_CQ 1U
#define MANA_HWC_RQ 2U
#define MANA_HWC_SQ 3U
#define MANA_HWC_RX 4U
#define MANA_HWC_TX 5U

struct mana_bar {
	uint64_t physical;
	uint64_t size;
};

struct mana_gdma_limits {
	uint32_t sq, rq, cq, eq, doorbells, mst;
	uint32_t cq_mod_contexts, mod_cq, msix;
};

/* Caller-owned storage must remain valid until mana_bootstrap_stop succeeds.
 * A failed stop retains mappings, IRQs, and DMA pages for a later retry.
 * Do not free or reuse quarantined storage after a timeout.
 */
struct mana_bootstrap {
	const struct mana_platform *p;
	struct mana_bar bars[6];
	struct mana_mapping bar0;
	struct mana_mapping table;
	struct mana_dma pages[MANA_HWC_PAGE_COUNT];
	struct mana_irq irq;
	uint64_t shm;
	uint64_t db;
	uint32_t db_size;
	uint32_t eq_id, cq_id, rq_id, sq_id, db_id;
	uint32_t pd_id, mkey, max_cqs, max_req, max_resp, depth;
	/* PF destination IDs from init fields 10 and 11. Sent in every SQ WQE. */
	uint32_t pf_dest_vrq_id, pf_dest_vrcq_id;
	uint32_t mana_device_id;
	struct mana_gdma_limits limits;
	uint32_t eq_head, cq_head, rq_head, sq_head;
	uint32_t init_seen;
	uint32_t rx_offset;
	volatile unsigned int interrupts;
	uint32_t msix_saved[4];
	uint32_t bar_saved[6], bar_dirty;
	uint16_t command_saved, msix_saved_control, msix_cap;
	uint64_t table_offset;
	uint64_t table_length, pba_offset, pba_length;
	uint32_t table_bir, pba_bir;
	int error;
	unsigned int config_owned, irq_owned, table_owned, dma_exposed;
	unsigned int initialized, ready, quarantined, test_event;
	unsigned int destroy_sent, hwc_destroyed;
	unsigned int pending, rx_done, tx_done;
	uint32_t pending_type, pending_activity, pending_device;
	uint16_t pending_response_version;
	unsigned int control_owned, link_known, link_up;
	uint32_t link_generation;
	void *response;
	size_t response_size, response_length;
};

int mana_bootstrap_start(struct mana_bootstrap *b, const struct mana_platform *p);
int mana_bootstrap_stop(struct mana_bootstrap *b);
int mana_bootstrap_poll(struct mana_bootstrap *b);
int mana_hwc_request(struct mana_bootstrap *b, const void *request,
		     size_t request_size, void *response, size_t response_size,
		     size_t *response_length);

/* Unikraft-only entry points. No device means a NULL bootstrap pointer. */
struct mana_bootstrap *mana_unikraft_bootstrap(void);
int mana_unikraft_stop(void);

#endif
