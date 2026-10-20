/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_BOOTSTRAP_H
#define MANA_HOST_BOOTSTRAP_H

#include "mock.h"
#include <uk/mana/bootstrap.h>

/* PF destination IDs the model sends in init fields 10 and 11. The model
 * checks that both values reach the SQ WQE destination fields.
 */
#define HOST_BOOTSTRAP_INIT_PF_DEST_RQ_ID 12U
#define HOST_BOOTSTRAP_INIT_PF_DEST_CQ_ID 13U

enum host_bootstrap_fault {
	HOST_BOOTSTRAP_NORMAL,
	HOST_BOOTSTRAP_SMC_BUSY,
	HOST_BOOTSTRAP_SMC_STATUS,
	HOST_BOOTSTRAP_NO_INIT,
	HOST_BOOTSTRAP_BAD_OWNER,
	HOST_BOOTSTRAP_DUP_INIT,
	HOST_BOOTSTRAP_SMALL_REQUEST,
	HOST_BOOTSTRAP_BAD_DB,
	HOST_BOOTSTRAP_BAD_CQ,
	HOST_BOOTSTRAP_BAD_LENGTH,
	HOST_BOOTSTRAP_BAD_MESSAGE_ID,
	HOST_BOOTSTRAP_BAD_STATUS,
	HOST_BOOTSTRAP_NO_TX,
	HOST_BOOTSTRAP_NO_IRQ,
	HOST_BOOTSTRAP_NO_MANA,
	HOST_BOOTSTRAP_BAD_VERSION,
	HOST_BOOTSTRAP_DESTROY_TIMEOUT,
	HOST_BOOTSTRAP_UNKNOWN_EVENT,
	HOST_BOOTSTRAP_MISSING_INIT,
	HOST_BOOTSTRAP_WRONG_QUEUE,
	HOST_BOOTSTRAP_SHORT_RESPONSE,
	HOST_BOOTSTRAP_BAD_SMC_TYPE,
};

/* Return -ENOTSUP to use built-in responses. Other errors abort the adapter. */
typedef int (*host_bootstrap_response_fn)(
	void *ctx, const unsigned char *request, size_t request_length,
	unsigned char *response, size_t response_capacity, size_t *response_length);

struct host_bootstrap {
	struct host_mock host;
	struct mana_platform platform;
	struct mana_bootstrap b;
	unsigned char config[256];
	unsigned char table_saved[16];
	enum host_bootstrap_fault fault;
	void (*handler)(void *);
	void *arg;
	unsigned int irq_live, maps, errors, smc_destroy, requests;
	/* Bootstrap RQ, CQ, and EQ doorbells checked by write64. */
	unsigned int rq_bells, cq_bells, eq_bells;
	unsigned int cfg_calls, cfg_fail_at, map_fail, irq_fail, unmap_fail;
	unsigned int irq_free_fail, dma_bad_at, dma_calls, wide_bar, clock_fail;
	/* Report the n-th bootstrap page at dma_high_value instead of the
	 * allocator address. The tracked slot keeps the reported value so a
	 * later free still matches.
	 */
	unsigned int dma_high_at;
	uint64_t dma_high_value;
	/* 1: MSI-X Message Control writes are silently dropped. */
	unsigned int msix_control_stuck;
	uint32_t eq_producer, cq_producer;
	int mismatch;
	host_bootstrap_response_fn respond;
	void *respond_ctx;
};

void host_bootstrap_init(struct host_bootstrap *model,
			 enum host_bootstrap_fault fault);
uint32_t host_bootstrap_config(const struct host_bootstrap *model,
			      unsigned int offset, unsigned int width);
/* Complete the doorbelled control request. Used by the synchronous model and
 * by the asynchronous device model at the scheduled device time.
 */
void host_bootstrap_request(struct host_bootstrap *model);
/* Write one bootstrap EQ event with correct owner bits at the model
 * producer. The asynchronous device model schedules this at a later time.
 */
void host_bootstrap_event(struct host_bootstrap *model, uint32_t type,
			  uint32_t data);

#endif
