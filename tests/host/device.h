/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Asynchronous host device model for the MANA driver tests.
 *
 * The model wraps the synchronous bootstrap transport. A doorbell records
 * the write and schedules timed device work. The device produces entries
 * later, on its own clock:
 *   - host_device_tick() moves the device clock forward, or
 *   - the driver's delay_us() hook advances it inside polling loops.
 * This proves the driver waits for asynchronous completion instead of
 * assuming a doorbell completes synchronously.
 *
 * Limits:
 *   - One model per fixture. The model runs inline. It starts no threads.
 *   - Time advances only through explicit calls. There is no wall clock.
 *   - Payload responses stay in the fixture. The model schedules the
 *     device-side entry work at the latency settings.
 *   - The model writes device rings through the driver-visible queue
 *     storage, like the synchronous fixtures do.
 */
#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "mock.h"
#include <uk/mana/data_queue.h>

#define DEVICE_WORK_SLOTS 64U
#define DEVICE_FRAME_MAX 256U

/* Scheduled device work kinds. */
enum device_work_kind {
	DEVICE_WORK_NONE,
	DEVICE_WORK_CTRL,     /* Complete the doorbelled control request. */
	DEVICE_WORK_EQE,      /* Produce one data EQ entry. */
	DEVICE_WORK_TX,       /* Drain TX WQ entries and complete them. */
	DEVICE_WORK_RX,       /* Deliver one frame into one RX buffer. */
	DEVICE_WORK_FENCE,    /* Produce the RX fence CQE. */
	DEVICE_WORK_BOOT_EQE, /* Produce one bootstrap EQ event. */
};

/* One scheduled work item. due_us is the device clock deadline. */
struct device_work {
	uint64_t due_us;
	uint32_t type;  /* EQE type for EQE and bootstrap work. */
	uint32_t data;  /* EQE data word. */
	uint32_t size;  /* RX frame size. */
	enum device_work_kind kind;
	bool used;      /* The slot holds work. */
	bool retried;   /* A ring overrun already rescheduled this slot. */
};

/* One bad doorbell the model could not route. */
struct device_doorbell {
	uint64_t offset;
	uint64_t value;
};

/* One data queue seen by the device. The queue ids arrive with the
 * CREATE_WQ response, before the test binds driver storage.
 */
struct device_queue {
	struct mana_data_queue *queue; /* Bound by host_device_note_queue. */
	uint32_t wq_id, cq_id;
	unsigned int exists;           /* Created and not yet destroyed. */
};

struct host_device {
	struct host_bootstrap *model;
	void (*inner_write64)(void *ctx, const struct mana_mapping *map,
			      uint64_t offset, uint64_t value);
	int (*inner_delay)(void *ctx, unsigned int usec);
	host_bootstrap_response_fn inner_respond;
	void *inner_respond_ctx;
	bool attached;

	uint64_t now_us; /* Device clock, separate from the mock clock. */

	unsigned int ctrl_latency_us;  /* Default: 5. */
	unsigned int data_latency_us;  /* Default: 8. */
	unsigned int event_latency_us; /* Default: 10. */

	struct device_work work[DEVICE_WORK_SLOTS];
	unsigned int work_scan;   /* Ring cursor for the deadline scan. */
	unsigned int work_count;  /* Slots holding work. */
	unsigned int ctrl_pends;  /* Times the ctrl slot was double armed. */
	unsigned int work_drops;  /* Work dropped after a queue teardown. */
	unsigned int eq_guards;   /* Overrun reschedules of data EQ writes. */
	unsigned int cq_guards;   /* Overrun reschedules of CQ writes. */

	struct mana_data *data;             /* Data EQ, noted after start. */
	struct device_queue queue[2];       /* [0] TX, [1] RX. */
	uint32_t eq_producer;
	uint32_t cq_producer[2];
	uint32_t wq_next[2];    /* WQ units the device has consumed. */
	uint32_t wq_bell[2];    /* Byte head from the last doorbell. */

	/* Injection controls. Each applies once unless noted. */
	unsigned int stalled; /* 1: hold data work, 2: release and resume. */
	bool hold_tx;         /* Skip the next TX WQE instead of completing. */
	bool duplicate_tx;    /* Write a second TX CQE for one WQE. */
	uint32_t late_us;     /* Extra latency for the next data completion. */
	uint32_t error_vendor_left; /* Vendor error for the next TX CQE. */
	bool rx_drop;         /* Mark the next RX CQE as a dropped frame. */
	enum device_cqe_fault {
		DEVICE_CQE_FAULT_NONE,
		DEVICE_CQE_FAULT_BAD_ID,        /* Wrong WQ id field. */
		DEVICE_CQE_FAULT_BAD_CLIENT,    /* Client type bits broken. */
		DEVICE_CQE_FAULT_BAD_OWNER,     /* Owner generation advanced. */
		DEVICE_CQE_FAULT_BAD_DIRECTION, /* SQ bit inverted. */
	} cqe_fault;

	/* Staging for the scheduled RX frame. */
	unsigned char inject_frame[DEVICE_FRAME_MAX];

	/* Captures for assertions. */
	uint32_t tx_frames;             /* TX WQE entries processed. */
	uint32_t last_tx_length;        /* Wire length of the last frame. */
	unsigned char last_tx_frame[DEVICE_FRAME_MAX];
	struct device_doorbell last_bad_doorbell;
	uint32_t bad_doorbells;
};

/* Route doorbells, delays, and responses through the model. Call before
 * mana_bootstrap_start. Latencies default on attach.
 */
void host_device_attach(struct host_bootstrap *model, struct host_device *dev);
/* Stop routing. The model must be fully stopped first. */
void host_device_detach(struct host_device *dev);
/* Default latencies: control 5, data 8, event 10 microseconds. */
void host_device_set_latency(struct host_device *dev, unsigned int ctrl_us,
			     unsigned int data_us, unsigned int event_us);
/* Register driver rings. Note data before mana_data_start, because the
 * device answers the EQ self-test inside that call. Bind queue storage
 * after create; the device learns queue ids from the CREATE_WQ exchange.
 */
void host_device_note_data(struct host_device *dev, struct mana_data *data);
void host_device_note_queue(struct host_device *dev, struct mana_data_queue *q);
void host_device_forget_queue(struct host_device *dev, struct mana_data_queue *q);

/* Advance the device clock and run every work item that became due. */
void host_device_tick(struct host_device *dev, uint64_t us);
/* Schedule one data EQ entry with the given type and data word. */
int host_device_eqe(struct host_device *dev, uint32_t type, uint32_t data);
/* Schedule one bootstrap EQ event, for example type 133 link state. */
int host_device_bootstrap_event(struct host_device *dev, uint32_t type,
				uint32_t data);
/* Schedule one RX frame delivery into the oldest posted RX buffer. */
int host_device_rx_inject(struct host_device *dev, const void *frame,
			  size_t size);

/* Fault injections. Each arms one use-once behaviour. */
void host_device_stall_data(struct host_device *dev, bool on);
void host_device_hold_tx(struct host_device *dev, bool on);
void host_device_duplicate_tx(struct host_device *dev);
void host_device_late(struct host_device *dev, uint32_t extra_us);
void host_device_error_vendor(struct host_device *dev, uint32_t vendor_left);
void host_device_rx_drop(struct host_device *dev);
void host_device_cqe_fault(struct host_device *dev,
			   enum device_cqe_fault fault);

#endif /* HOST_DEVICE_H */
