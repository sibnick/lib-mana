/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_DATA_QUEUE_H
#define UK_MANA_DATA_QUEUE_H

#include <uk/mana/control.h>

#define MANA_DATA_COMPLETION_BYTES 60U
#define MANA_DATA_MAX_SGE 30U

struct mana_data;

struct mana_data_region {
	struct mana_dma dma;
	uint64_t handle, original_handle;
	unsigned int registered, consumed, quarantined;
};

struct mana_data_queue {
	struct mana_data *data;
	struct mana_control_queue object;
	struct mana_data_region wq, cq;
	uint32_t head, tail, cq_head, retired_cq_id;
	unsigned int stopped, notified, object_destroyed;
};

/* One separate EQ shares the bootstrap's MSI-X vector zero. Serialize all
 * calls with bootstrap, control, and datapath calls outside IRQ context.
 * Use fresh, zeroed storage. Keep storage and callbacks until stop succeeds.
 * A failed start or create can own resources. Call stop or destroy unless
 * quarantined. Quarantine has no safe release API.
 */
struct mana_data {
	struct mana_control *control;
	struct mana_data_region eq;
	struct mana_data_queue *queues[2];
	uint32_t eq_id, eq_head, regions;
	unsigned int eq_live, ready, quarantined, test_seen;
};

struct mana_data_sge {
	uint64_t address;
	uint32_t memory_key, size;
};

/* No OOB-in-SGL, padding, chaining, or packet offload support.
 * SQ uses 8 or 24 OOB bytes. RQ uses zero OOB bytes and data_unit zero.
 * SGE memory stays device-owned until the datapath proves completion.
 */
struct mana_data_work {
	const struct mana_data_sge *sges;
	uint32_t num_sge, data_unit;
	const void *oob;
	uint32_t oob_size;
};

struct mana_data_posted {
	uint32_t offset; /* Byte offset within the WQ ring. */
	uint32_t basic_units;
};

enum mana_data_completion_kind {
	MANA_DATA_COMPLETION,
	MANA_DATA_RX_FENCE
};

struct mana_data_completion {
	uint32_t wq_id;
	unsigned int is_sq;
	enum mana_data_completion_kind kind;
	unsigned char data[MANA_DATA_COMPLETION_BYTES];
};

int mana_data_start(struct mana_data *d, struct mana_control *c,
		    uint32_t eq_size);
int mana_data_stop(struct mana_data *d);
/* Process at most one EQ ring. CQ notifications set queue.notified.
 * Poll each live CQ independently. IRQ callbacks only record delivery.
 */
int mana_data_poll(struct mana_data *d);
int mana_data_queue_create(struct mana_data *d, struct mana_data_queue *q,
			   enum mana_control_queue_type type,
			   uint32_t wq_size, uint32_t cq_size);
int mana_data_queue_destroy(struct mana_data_queue *q);
int mana_data_queue_quiesce(struct mana_data_queue *q);
int mana_data_post(struct mana_data_queue *q, const struct mana_data_work *work,
		   struct mana_data_posted *posted);
/* Retire only a complete, oldest WQE after validating its packet CQ payload.
 * The datapath keeps the FIFO of posted offsets and basic-unit counts.
 * RX fence does not retire outstanding buffers. Destroy releases them.
 */
int mana_data_retire(struct mana_data_queue *q,
		     const struct mana_data_posted *posted);
/* Return 1 for one completion, 0 for empty, or a negative errno.
 * Only a validated RX fence changes control fence state.
 * Other packet completion decoding remains the datapath's responsibility.
 */
int mana_data_poll_cq(struct mana_data_queue *q,
		      struct mana_data_completion *completion);

#endif
