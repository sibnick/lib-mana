/* SPDX-License-Identifier: BSD-3-Clause */
#include "device.h"
#include "../../src/internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One model routes through one fixture at a time. The platform hooks
 * receive only the bootstrap context, so the active fixture pointer lives
 * here. Attach before starting the next fixture and detach after stopping
 * it. Tests run one fixture at a time.
 */
static struct host_device *active_model;

#define DIAG_VALUE 0xabcdef98U

static int dma_find(struct host_device *dev, uint64_t device_addr,
		    unsigned char **payload, uint64_t *span)
{
	struct host_mock *mock = &dev->model->host;
	unsigned int i;

	for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++) {
		if (mock->dma[i].device_address != device_addr ||
		    !mock->dma[i].address)
			continue;
		*payload = mock->dma[i].address;
		*span = mock->dma[i].size;
		return 1;
	}
	return 0;
}

static int schedule(struct host_device *dev, enum device_work_kind kind,
		    unsigned int latency_us, uint32_t type, uint32_t data,
		    uint32_t size)
{
	unsigned int i;

	for (i = 0; i < DEVICE_WORK_SLOTS; i++) {
		struct device_work *slot = &dev->work[i];

		if (slot->used)
			continue;
		slot->used = true;
		slot->retried = false;
		slot->kind = kind;
		slot->type = type;
		slot->data = data;
		slot->size = size;
		slot->due_us = dev->now_us + latency_us;
		dev->work_count++;
		return 0;
	}
	dev->work_drops++;
	return -ENOSPC;
}

static unsigned int data_latency(struct host_device *dev)
{
	unsigned int latency = dev->data_latency_us + dev->late_us;

	dev->late_us = 0;
	return latency;
}

static int kind_is_data(enum device_work_kind kind)
{
	return kind == DEVICE_WORK_EQE || kind == DEVICE_WORK_TX ||
		kind == DEVICE_WORK_RX || kind == DEVICE_WORK_FENCE;
}

static void raise_interrupt(struct host_device *dev)
{
	if (dev->model->handler)
		dev->model->handler(dev->model->arg);
}

/* Write one data EQ entry. Return 1 on success, 0 on an overrun retry. */
static int write_eqe(struct host_device *dev, uint32_t type, uint32_t data)
{
	struct mana_data *d = dev->data;
	uint32_t entries, pending;
	unsigned char *entry;

	if (!d || !d->eq_live || !d->eq.dma.addr) {
		dev->work_drops++;
		return 1;
	}
	entries = (uint32_t)d->eq.dma.size / 16;
	if (!dev->eq_producer)
		dev->eq_producer = entries; /* Match the driver eq_head prime. */
	pending = dev->eq_producer - d->eq_head;
	if (pending >= entries && !dev->stalled) {
		dev->eq_guards++;
		return 0;
	}
	entry = (unsigned char *)d->eq.dma.addr +
		(dev->eq_producer % entries) * 16;
	memset(entry, 0, 16);
	mana_put32(entry, data);
	mana_put32(entry + 12, type | ((dev->eq_producer / entries) & 7) << 29);
	dev->eq_producer++;
	raise_interrupt(dev);
	return 1;
}

/* Write one CQE on the queue at the device-side producer. The caller
 * supplies the first two words and the info field. Return 0 on overrun.
 */
static int write_cqe(struct host_device *dev, unsigned int which,
		     uint32_t header, uint32_t word4, uint32_t word8,
		     uint32_t word12, uint32_t info)
{
	struct mana_data_queue *q = dev->queue[which].queue;
	uint32_t entries, pending, generation = 0;
	unsigned char *entry;

	if (!q || !q->object.live || !q->cq.dma.addr) {
		dev->work_drops++;
		return 1;
	}
	entries = (uint32_t)q->cq.dma.size / 64;
	pending = dev->cq_producer[which] - q->cq_head;
	if (pending >= entries) {
		dev->cq_guards++;
		return 0;
	}
	if (dev->cqe_fault == DEVICE_CQE_FAULT_BAD_OWNER) {
		generation = 1;
		dev->cqe_fault = DEVICE_CQE_FAULT_NONE;
	}
	entry = (unsigned char *)q->cq.dma.addr +
		(dev->cq_producer[which] % entries) * 64;
	memset(entry, 0, 64);
	if (dev->cqe_fault == DEVICE_CQE_FAULT_BAD_CLIENT) {
		header &= ~0x40U;
		dev->cqe_fault = DEVICE_CQE_FAULT_NONE;
	}
	if (dev->cqe_fault == DEVICE_CQE_FAULT_BAD_ID) {
		info = (info & ~0xffffffU) | ((info + 100U) & 0xffffffU);
		dev->cqe_fault = DEVICE_CQE_FAULT_NONE;
	}
	if (dev->cqe_fault == DEVICE_CQE_FAULT_BAD_DIRECTION) {
		info ^= 1U << 24;
		dev->cqe_fault = DEVICE_CQE_FAULT_NONE;
	}
	mana_put32(entry, header);
	mana_put32(entry + 4, word4);
	mana_put32(entry + 8, word8);
	mana_put32(entry + 12, word12);
	mana_put32(entry + 56, DIAG_VALUE);
	mana_put32(entry + 60, info |
		   ((((dev->cq_producer[which] / entries) & 7) + generation) &
		    7) << 29);
	dev->cq_producer[which]++;
	return 1;
}

static int produce_tx(struct host_device *dev)
{
	struct mana_data_queue *q = dev->queue[0].queue;
	unsigned char *ring;
	uint32_t ring_bytes;

	if (!q || !q->object.live || !q->wq.dma.addr) {
		dev->work_drops++;
		return 1;
	}
	ring = (unsigned char *)q->wq.dma.addr;
	ring_bytes = (uint32_t)q->wq.dma.size;
	while (dev->wq_next[0] * 32U < dev->wq_bell[0]) {
		unsigned char *wqe = ring + (dev->wq_next[0] * 32U) % ring_bytes;
		uint32_t head = mana_get32(wqe + 4);
		uint32_t sge_count = head & 0xff, oob = (head >> 8) & 0xff;
		uint32_t offset = dev->wq_next[0] * 32U % ring_bytes;
		unsigned char *payload = NULL;
		uint64_t span = 0;
		uint32_t length, units, info;
		uint32_t header = 0x60;
		int err;

		units = (8U + oob * 4U + sge_count * 16U + 31U) & ~31U;
		length = mana_get32(wqe + 8 + oob * 4U + 12);
		if (dev->hold_tx) {
			dev->hold_tx = false;
			return 1;
		}
		if (dev->error_vendor_left) {
			header |= dev->error_vendor_left << 8;
			dev->error_vendor_left = 0;
		}
		if (!dma_find(dev, mana_get64(wqe + 8 + oob * 4U), &payload,
			      &span)) {
			dev->model->mismatch = 1;
			dev->wq_next[0] += units / 32;
			continue;
		}
		info = q->object.wq_id | (1U << 24);
		err = write_cqe(dev, 0, header, 0, (offset >> 5) << 5 | 1, 0,
				info);
		if (!err)
			return 0;
		dev->tx_frames++;
		dev->last_tx_length = length;
		if (length <= DEVICE_FRAME_MAX)
			memcpy(dev->last_tx_frame, payload, length);
		else
			dev->model->mismatch = 1;
		dev->wq_next[0] += units / 32;
		if (dev->duplicate_tx) {
			dev->duplicate_tx = false;
			err = write_cqe(dev, 0, header, 0,
					(offset >> 5) << 5 | 1, 0, info);
			if (!err)
				return 0;
		}
	}
	raise_interrupt(dev);
	return 1;
}

static int produce_rx(struct host_device *dev, const unsigned char *frame,
		      uint32_t size)
{
	struct mana_data_queue *q = dev->queue[1].queue;
	unsigned char *wqe, *payload = NULL;
	uint64_t span = 0;
	uint32_t head, sge_count, oob, units, info;
	uint32_t ring_bytes;
	int err;

	if (!q || !q->object.live || !q->wq.dma.addr) {
		dev->work_drops++;
		return 1;
	}
	if (dev->wq_next[1] * 32U >= dev->wq_bell[1]) {
		dev->work_drops++;
		return 1;
	}
	ring_bytes = (uint32_t)q->wq.dma.size;
	wqe = (unsigned char *)q->wq.dma.addr +
		(dev->wq_next[1] * 32U) % ring_bytes;
	head = mana_get32(wqe + 4);
	sge_count = head & 0xff;
	oob = (head >> 8) & 0xff;
	units = ((8U + oob * 4U + sge_count * 16U + 31U) & ~31U) / 32;
	if (!dma_find(dev, mana_get64(wqe + 8 + oob * 4U), &payload, &span) ||
	    span < size) {
		dev->model->mismatch = 1;
		return 1;
	}
	memcpy(payload, frame, size);
	info = q->object.wq_id;
	err = write_cqe(dev, 1, dev->rx_drop ? 0x44U : 0x41U, 0, size, 0, info);
	if (!err)
		return 0;
	dev->rx_drop = false;
	dev->wq_next[1] += units;
	if (write_eqe(dev, 3, q->object.cq_id) && q->object.live)
		raise_interrupt(dev);
	return 1;
}

/* Run one due work item. Return false when a ring guard asks for a retry. */
static int run_work(struct host_device *dev, struct device_work *work)
{
	unsigned char frame[DEVICE_FRAME_MAX];

	switch (work->kind) {
	case DEVICE_WORK_CTRL:
		host_bootstrap_request(dev->model);
		return 1;
	case DEVICE_WORK_EQE:
		return write_eqe(dev, work->type, work->data);
	case DEVICE_WORK_TX:
		return produce_tx(dev);
	case DEVICE_WORK_RX:
		if (work->size > DEVICE_FRAME_MAX) {
			dev->model->mismatch = 1;
			return 1;
		}
		memcpy(frame, dev->inject_frame, work->size);
		return produce_rx(dev, frame, work->size);
	case DEVICE_WORK_FENCE:
		if (!dev->queue[1].queue || !dev->queue[1].queue->object.live) {
			dev->work_drops++;
			return 1;
		}
		if (!write_cqe(dev, 1, 0x43U, 0, 0, 0,
			       dev->queue[1].queue->object.wq_id))
			return 0;
		raise_interrupt(dev);
		return 1;
	case DEVICE_WORK_BOOT_EQE: {
		struct host_bootstrap *f = dev->model;
		struct mana_bootstrap *b = &f->b;

		if (b->dma_exposed && !b->quarantined &&
		    f->eq_producer - b->eq_head >= 256) {
			dev->eq_guards++;
			return 0;
		}
		host_bootstrap_event(f, work->type, work->data);
		raise_interrupt(dev);
		return 1;
	}
	case DEVICE_WORK_NONE:
	default:
		return 1;
	}
}

static void process_due(struct host_device *dev)
{
	unsigned int pass;

	for (pass = 0; pass < 4; pass++) {
		struct device_work *picked = NULL;
		unsigned int n;

		for (n = 0; n < DEVICE_WORK_SLOTS; n++) {
			struct device_work *work =
				&dev->work[(dev->work_scan + n) % DEVICE_WORK_SLOTS];

			if (!work->used || work->due_us > dev->now_us)
				continue;
			if (dev->stalled && kind_is_data(work->kind))
				continue;
			if (work->retried)
				continue;
			picked = work;
			dev->work_scan =
				(dev->work_scan + n + 1) % DEVICE_WORK_SLOTS;
			break;
		}
		if (!picked)
			break;
		if (run_work(dev, picked)) {
			picked->used = false;
			picked->kind = DEVICE_WORK_NONE;
			dev->work_count--;
		} else {
			picked->retried = true;
		}
	}
}

static void route_doorbell(struct host_device *dev, uint64_t offset,
			   uint64_t value)
{
	uint32_t id = (uint32_t)value & 0xffffff;
	struct host_bootstrap *f = dev->model;
	unsigned int i;

	if (offset == 0x1000 && id == f->b.sq_id) {
		if ((uint32_t)(value >> 32) != f->b.sq_head * 32)
			f->mismatch = 1;
		for (i = 0; i < DEVICE_WORK_SLOTS; i++) {
			if (dev->work[i].used &&
			    dev->work[i].kind == DEVICE_WORK_CTRL) {
				dev->ctrl_pends++;
				return;
			}
		}
		schedule(dev, DEVICE_WORK_CTRL, dev->ctrl_latency_us, 0, 0, 0);
		return;
	}
	for (i = 0; i < 2; i++) {
		if (dev->queue[i].exists && id == dev->queue[i].wq_id) {
			dev->wq_bell[i] = (uint32_t)(value >> 32);
			if (i == 0)
				schedule(dev, DEVICE_WORK_TX,
					 data_latency(dev), 0, 0, 0);
			return;
		}
	}
	for (i = 0; i < 2; i++) {
		if (dev->queue[i].exists && id == dev->queue[i].cq_id)
			return; /* CQ arm. The device ignores it. */
	}
	if (dev->data && id == dev->data->eq_id)
		return; /* Data EQ arm. The device ignores it. */
	if (id == f->b.rq_id) {
		if ((uint32_t)(value >> 32) != f->b.rq_head * 32 ||
		    value >> 63)
			f->mismatch = 1;
		return;
	}
	if (id == f->b.cq_id) {
		if ((uint32_t)(value >> 32) !=
		    (f->b.cq_head % 512 | 0x80000000U))
			f->mismatch = 1;
		return;
	}
	if (id == f->b.eq_id) {
		if ((uint32_t)(value >> 32) !=
		    (f->b.eq_head % 2048 | 0x80000000U))
			f->mismatch = 1;
		return;
	}
	dev->bad_doorbells++;
	dev->last_bad_doorbell.offset = offset;
	dev->last_bad_doorbell.value = value;
}

static void dev_write64(void *ctx, const struct mana_mapping *map,
			uint64_t offset, uint64_t value)
{
	struct host_device *dev = active_model;
	struct host_bootstrap *f = ctx;
	int err;

	if (map->addr != f->host.bar)
		abort();
	err = host_mock_write(&f->host, (size_t)offset, 8, value);
	if (err) {
		fprintf(stderr, "mock doorbell write failed: %d\n", err);
		abort();
	}
	if (!dev || !dev->attached)
		return;
	route_doorbell(dev, offset, value);
}

static int dev_delay(void *ctx, unsigned int usec)
{
	struct host_device *dev = active_model;
	int err;

	err = dev->inner_delay(ctx, usec);
	if (err)
		return err;
	if (!dev->attached)
		return 0;
	dev->now_us += usec;
	process_due(dev);
	return 0;
}

static int dev_respond(void *ctx, const unsigned char *req, size_t size,
		       unsigned char *resp, size_t capacity, size_t *length)
{
	struct host_device *dev = active_model;
	int err;

	(void)ctx;
	if (!dev || !dev->inner_respond)
		return -ENOTSUP;
	err = dev->inner_respond(dev->inner_respond_ctx, req, size, resp,
				 capacity, length);
	if (err || !dev->attached || mana_get32(resp + 24))
		return err;
	switch (mana_get32(req + 4)) {
	case 10: /* The data EQ self-test asks for one data EQE. */
		if (mana_get32(req + 32) != 1)
			schedule(dev, DEVICE_WORK_EQE, data_latency(dev), 64,
				 0, 0);
		break;
	case 0x20004: /* Track the created queue identifiers. */
		{
			uint32_t slot = mana_get32(req + 48) - 1;

			if (slot < 2) {
				dev->queue[slot].exists = 1;
				dev->queue[slot].wq_id = mana_get32(resp + 32);
				dev->queue[slot].cq_id = mana_get32(resp + 36);
			}
		}
		break;
	case 0x20005: /* The destroyed queue leaves the device. */
		{
			uint32_t slot = mana_get32(req + 40) - 1;

			if (slot < 2)
				dev->queue[slot].exists = 0;
		}
		break;
	case 0x20006: /* The RX fence command asks for one fence CQE. */
		schedule(dev, DEVICE_WORK_FENCE, data_latency(dev), 0, 0, 0);
		break;
	default:
		break;
	}
	return err;
}

void host_device_set_latency(struct host_device *dev, unsigned int ctrl_us,
			     unsigned int data_us, unsigned int event_us)
{
	dev->ctrl_latency_us = ctrl_us;
	dev->data_latency_us = data_us;
	dev->event_latency_us = event_us;
}

void host_device_attach(struct host_bootstrap *model, struct host_device *dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->model = model;
	dev->attached = true;
	dev->ctrl_latency_us = 5;
	dev->data_latency_us = 8;
	dev->event_latency_us = 10;
	dev->inner_write64 = model->platform.write64;
	dev->inner_delay = model->platform.delay_us;
	dev->inner_respond = model->respond;
	dev->inner_respond_ctx = model->respond_ctx;
	model->respond = dev_respond;
	model->respond_ctx = model; /* Unused. dev_respond uses the active. */
	model->platform.write64 = dev_write64;
	model->platform.delay_us = dev_delay;
	active_model = dev;
}

void host_device_detach(struct host_device *dev)
{
	struct host_bootstrap *model = dev->model;

	if (!dev->attached)
		return;
	model->platform.write64 = dev->inner_write64;
	model->platform.delay_us = dev->inner_delay;
	model->respond = dev->inner_respond;
	model->respond_ctx = dev->inner_respond_ctx;
	dev->attached = false;
	if (active_model == dev)
		active_model = NULL;
}

void host_device_note_data(struct host_device *dev, struct mana_data *data)
{
	dev->data = data;
	dev->eq_producer = 0; /* Primes lazily at the first EQ write. */
}

void host_device_note_queue(struct host_device *dev, struct mana_data_queue *q)
{
	unsigned int slot = q->object.type == MANA_CONTROL_TX ? 0 : 1;

	dev->queue[slot].queue = q;
	dev->cq_producer[slot] = (uint32_t)q->cq.dma.size / 64;
	if (!dev->queue[slot].exists) {
		dev->queue[slot].exists = 1;
		dev->queue[slot].wq_id = q->object.wq_id;
		dev->queue[slot].cq_id = q->object.cq_id;
	}
}

void host_device_forget_queue(struct host_device *dev, struct mana_data_queue *q)
{
	unsigned int i;

	/* Match the stored pointer. The driver zeroes the queue struct
	 * during destroy, so its type field is no longer trustworthy here.
	 */
	for (i = 0; i < 2; i++)
		if (dev->queue[i].queue == q) {
			dev->queue[i].queue = NULL;
			dev->queue[i].exists = 0;
		}
}

void host_device_tick(struct host_device *dev, uint64_t us)
{
	dev->now_us += us;
	process_due(dev);
}

int host_device_eqe(struct host_device *dev, uint32_t type, uint32_t data)
{
	return schedule(dev, DEVICE_WORK_EQE, dev->event_latency_us, type,
			data, 0);
}

int host_device_bootstrap_event(struct host_device *dev, uint32_t type,
				uint32_t data)
{
	return schedule(dev, DEVICE_WORK_BOOT_EQE, dev->event_latency_us, type,
			data, 0);
}

int host_device_rx_inject(struct host_device *dev, const void *frame,
			  size_t size)
{
	if (!size || size > DEVICE_FRAME_MAX)
		return -EINVAL;
	memcpy(dev->inject_frame, frame, size);
	return schedule(dev, DEVICE_WORK_RX, data_latency(dev), 0, 0,
			(uint32_t)size);
}

void host_device_stall_data(struct host_device *dev, bool on)
{
	dev->stalled = on ? 1 : 0;
}

void host_device_hold_tx(struct host_device *dev, bool on)
{
	dev->hold_tx = on;
}

void host_device_duplicate_tx(struct host_device *dev)
{
	dev->duplicate_tx = true;
}

void host_device_late(struct host_device *dev, uint32_t extra_us)
{
	dev->late_us = extra_us;
}

void host_device_error_vendor(struct host_device *dev, uint32_t vendor_left)
{
	dev->error_vendor_left = vendor_left;
}

void host_device_rx_drop(struct host_device *dev)
{
	dev->rx_drop = true;
}

void host_device_cqe_fault(struct host_device *dev, enum device_cqe_fault fault)
{
	dev->cqe_fault = fault;
}
