/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_TX_H
#define UK_MANA_TX_H

#include <uk/mana/data_queue.h>

#define MANA_TX_DRAIN_POLLS 10000U

struct mana_tx_record {
	struct mana_dma dma;
	struct mana_data_posted posted;
	void *cookie;
	uint32_t length, wire_length;
};

struct mana_tx_result {
	void *cookie;
	uint32_t length, wire_length;
	uint32_t type, vendor_error;
	/* The source consumer does not validate these device diagnostic fields. */
	uint32_t data_offset, sgl_offset, wqe_offset;
	int error;
};

/* Supply fresh, zeroed storage and a record array. Retain both until stop
 * succeeds. Serialize calls with data, control, and bootstrap operations.
 * Do not change records or the embedded queue while TX owns them.
 * Quarantine retains pending DMA and cookies. It has no release API.
 */
struct mana_tx {
	struct mana_data *data;
	struct mana_data_queue queue;
	struct mana_tx_record *records;
	uint32_t capacity, first, count, oob_size;
	unsigned int ready, stopped, quarantined;
};

typedef void (*mana_tx_complete_fn)(void *ctx,
				    const struct mana_tx_result *result);

int mana_tx_start(struct mana_tx *tx, struct mana_data *data,
		  struct mana_tx_record *records, uint32_t capacity,
		  uint32_t wq_size, uint32_t cq_size);
/* Return 1 for accepted or a negative errno without accepting the cookie.
 * Borrow frame bytes only during this call. Copy them into owned coherent
 * DMA and pad short Ethernet frames. The caller can then release its bytes.
 * Accepted cookies return exactly once through poll or the stop callback.
 * Quarantine does not return cookies whose DMA ownership remains uncertain.
 */
int mana_tx_submit(struct mana_tx *tx, const void *frame, uint32_t length,
		   void *cookie);
/* Return how many more complete packets submit can accept right now.
 * Count only proven capacity: free FIFO records and WQ space for whole
 * WQEs at the current WQE size. Return 0 before start, after stop, and
 * in quarantine. Any submit or poll changes the count. Serialize this
 * call with all other TX calls.
 */
uint32_t mana_tx_free_packets(const struct mana_tx *tx);
/* Return 1 for a result, 0 for empty, or a negative queue error.
 * result.error reports a known packet failure, not a queue poll failure.
 */
int mana_tx_poll(struct mana_tx *tx, struct mana_tx_result *result);
/* Quiesce, drain within ten seconds, and destroy the queue.
 * Require a callback while cookies remain. Do not reenter from callbacks.
 * Packet failures go to the callback. Return only a teardown error or zero.
 * A proven firmware destroy failure permits a retry. Timeout quarantines.
 * Stop also cleans up a failed start that still owns queue resources.
 */
int mana_tx_stop(struct mana_tx *tx, mana_tx_complete_fn complete, void *ctx);

#endif
