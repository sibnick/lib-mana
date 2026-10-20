/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_RX_H
#define UK_MANA_RX_H

#include <uk/mana/data_queue.h>

struct mana_rx_slot {
	struct mana_dma dma;
	struct mana_data_posted posted;
};

/* A successful poll transfers this allocation to the caller.
 * Do not copy ownership. Release exactly once through packet_release.
 * The saved allocator context must remain live until release.
 */
struct mana_rx_packet {
	struct mana_dma dma;
	uint32_t offset, length;
	void *release_ctx;
	void (*release)(void *, struct mana_dma *);
};

struct mana_rx {
	struct mana_data_queue queue;
	struct mana_rx_slot *slots;
	const struct mana_platform *platform;
	uint32_t capacity, first, count, buffer_size;
	unsigned int ready, stopping, quarantined;
	uint64_t delivered, dropped;
	/* unknown_completions counts CQEs with a type outside the pinned
	 * RX switch. The layer skips them without retiring a buffer,
	 * like mana_process_rx_cqe (mana_en.c:2382-2387).
	 */
	uint32_t unknown_completions;
};

enum mana_rx_result {
	MANA_RX_EMPTY = 0,
	MANA_RX_PACKET = 1,
	MANA_RX_DROPPED = 2,
	MANA_RX_FENCE = 3
};

/* Use fresh, zeroed RX and slot storage. Serialize with data/control calls.
 * Failed start can retain resources. Call stop unless quarantined.
 * CQ capacity must include one extra fence entry.
 */
int mana_rx_start(struct mana_rx *rx, struct mana_data *data,
		  struct mana_rx_slot *slots, uint32_t capacity,
		  uint32_t wq_size, uint32_t cq_size);
/* Poll one CQE. Packet output must own no allocation.
 * Refill explicitly after delivery or drop. Refill failure never revokes
 * an already delivered packet. Negative returns transfer no ownership.
 */
int mana_rx_poll(struct mana_rx *rx, struct mana_rx_packet *packet);
int mana_rx_refill(struct mana_rx *rx);
int mana_rx_packet_release(struct mana_rx_packet *packet);
/* Stop discards completed packets, waits for the data CQ fence, then destroys.
 * Delivered packets remain caller-owned. Quarantine has no release path.
 */
int mana_rx_stop(struct mana_rx *rx);

#endif
