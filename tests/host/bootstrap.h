/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_BOOTSTRAP_H
#define MANA_HOST_BOOTSTRAP_H

#include "mock.h"
#include <uk/mana/bootstrap.h>

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
	unsigned int cfg_calls, cfg_fail_at, map_fail, irq_fail, unmap_fail;
	unsigned int irq_free_fail, dma_bad_at, dma_calls, wide_bar, clock_fail;
	uint32_t eq_producer, cq_producer;
	int mismatch;
	host_bootstrap_response_fn respond;
	void *respond_ctx;
};

void host_bootstrap_init(struct host_bootstrap *model,
			 enum host_bootstrap_fault fault);
uint32_t host_bootstrap_config(const struct host_bootstrap *model,
			      unsigned int offset, unsigned int width);

#endif
