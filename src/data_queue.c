/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2021, Microsoft Corporation. */
/* Wire formats follow Linux MANA under its BSD-3-Clause license. */
#include "internal.h"
#include <uk/mana/data_queue.h>

#define DATA_TEST_POLLS 30000U

enum {
	GENERATE_TEST_EQE = 10,
	CREATE_QUEUE = 12,
	DISABLE_QUEUE = 13,
	CREATE_DMA_REGION = 25,
	DESTROY_DMA_REGION = 27,
	GDMA_EQ = 4
};

static int report(struct mana_data *d, const char *stage, int err)
{
	if (d && d->control && d->control->bootstrap)
		d->control->bootstrap->p->error(
			d->control->bootstrap->p->ctx, stage, err);
	return err;
}

static int poison(struct mana_data *d, const char *stage, int err)
{
	unsigned int i;
	d->quarantined = 1;
	d->ready = 0;
	d->eq.quarantined = 1;
	d->control->quarantined = 1;
	d->control->ready = 0;
	for (i = 0; i < 2; i++)
		if (d->queues[i]) {
			d->queues[i]->wq.quarantined = 1;
			d->queues[i]->cq.quarantined = 1;
			d->queues[i]->object.quarantined = 1;
		}
	return report(d, stage, err);
}

static int usable(struct mana_data *d)
{
	struct mana_control *c;
	if (!d || !d->control)
		return -EINVAL;
	c = d->control;
	if (d->quarantined || c->quarantined)
		return poison(d, "data quarantine", -ENOTCONN);
	if (c->bootstrap->quarantined || c->bootstrap->error ||
	    !c->bootstrap->ready)
		return poison(d, "data channel unavailable",
			      c->bootstrap->error ? c->bootstrap->error : -ENOTCONN);
	return 0;
}

static void header(unsigned char *req, uint32_t code, uint32_t in_size,
		   uint32_t out_size, uint32_t device)
{
	mana_put32(req + 4, code);
	mana_put16(req + 8, 1);
	mana_put32(req + 12, in_size);
	mana_put32(req + 20, code);
	mana_put16(req + 24, 1);
	mana_put32(req + 28, out_size);
	mana_put32(req + 32, device);
}

static int command(struct mana_data *d, const void *req, size_t in_size,
		    void *resp, size_t out_size)
{
	struct mana_bootstrap *b = d->control->bootstrap;
	size_t length = 0;
	int err = usable(d);
	if (err)
		return err;
	err = mana_hwc_request(b, req, in_size, resp, out_size, &length);
	if (b->error)
		return poison(d, "data command uncertain", err);
	if (err)
		return err;
	if (length != out_size)
		return poison(d, "data response length", -EPROTO);
	return 0;
}

static int valid_size(struct mana_data *d, uint32_t size)
{
	uint32_t max_req = d->control->bootstrap->max_req;
	return size >= 4096 && !(size & (size - 1)) &&
	       max_req >= 64 && size / 4096 <= (max_req - 64) / 8;
}

static int same_handle(const struct mana_data_region *a,
		       const struct mana_data_region *b)
{
	return a != b && b->dma.addr &&
	       a->original_handle == b->original_handle;
}

static int region_create(struct mana_data *d, struct mana_data_region *r,
			 uint32_t size)
{
	unsigned char req[4096] = { 0 }, resp[40] = { 0 };
	const struct mana_platform *p = d->control->bootstrap->p;
	uint32_t pages = size / 4096, i, in_size = 64 + pages * 8;
	int err;
	r->handle = 0;
	err = p->dma_alloc(p->ctx, size, &r->dma);
	if (err)
		return report(d, "data DMA allocation", err);
	d->regions++;
	if (!r->dma.addr || r->dma.size != size ||
	    ((uintptr_t)r->dma.addr & 4095) || (r->dma.device_addr & 4095) ||
	    r->dma.device_addr > UINT64_MAX - (size - 1)) {
		return report(d, "data DMA contract", -EPROTO);
	}
	memset(r->dma.addr, 0, size);
	header(req, CREATE_DMA_REGION, in_size, sizeof(resp), 0);
	mana_put64(req + 40, size);
	/* Offset zero and GDMA_PAGE_TYPE_4K (zero). */
	mana_put32(req + 56, pages);
	mana_put32(req + 60, pages);
	for (i = 0; i < pages; i++)
		mana_put64(req + 64 + i * 8, r->dma.device_addr + (uint64_t)i * 4096);
	err = command(d, req, in_size, resp, sizeof(resp));
	if (err) {
		if (d->quarantined)
			r->quarantined = 1;
		return err;
	}
	r->handle = mana_get64(resp + 32);
	r->original_handle = r->handle;
	r->registered = 1;
	if (!r->handle)
		return poison(d, "data region handle", -EPROTO);
	if (same_handle(r, &d->eq))
		return poison(d, "duplicate data region handle", -EPROTO);
	for (i = 0; i < 2; i++)
		if (d->queues[i] &&
		    (same_handle(r, &d->queues[i]->wq) ||
		     same_handle(r, &d->queues[i]->cq)))
			return poison(d, "duplicate data region handle", -EPROTO);
	return 0;
}

static int region_release(struct mana_data *d, struct mana_data_region *r)
{
	unsigned char req[48] = { 0 }, resp[32] = { 0 };
	const struct mana_platform *p = d->control->bootstrap->p;
	int err;
	if (r->quarantined || r->consumed)
		return report(d, "data region still owned", -EBUSY);
	if (r->registered) {
		header(req, DESTROY_DMA_REGION, sizeof(req), sizeof(resp), 0);
		mana_put64(req + 40, r->handle);
		err = command(d, req, sizeof(req), resp, sizeof(resp));
		if (err)
			return err;
		r->registered = 0;
	}
	if (r->dma.addr) {
		p->dma_free(p->ctx, &r->dma);
		d->regions--;
	}
	memset(r, 0, sizeof(*r));
	return 0;
}

static void consumed(struct mana_data_region *r)
{
	r->handle = 0;
	r->registered = 0;
	r->consumed = 1;
}

static void doorbell(struct mana_data *d, uint32_t offset, uint32_t id,
		     uint32_t head, unsigned int arm)
{
	struct mana_bootstrap *b = d->control->bootstrap;
	uint64_t value = id | (uint64_t)head << 32;
	if (arm)
		value |= UINT64_C(1) << 63;
	b->p->dma_wmb(b->p->ctx);
	b->p->write64(b->p->ctx, &b->bar0,
		      b->db + (uint64_t)b->db_size * d->control->doorbell_id +
		      offset, value);
}

static int owner(struct mana_data *d, const unsigned char *entry,
		 uint32_t head, uint32_t entries, uint32_t *info)
{
	uint32_t bits;
	*info = *(const volatile uint32_t *)entry;
	bits = *info >> 29;
	if (bits == ((head / entries - 1) & 7))
		return 0;
	if (bits != ((head / entries) & 7))
		return poison(d, "data ring owner overflow", -EOVERFLOW);
	d->control->bootstrap->p->dma_rmb(d->control->bootstrap->p->ctx);
	return 1;
}

/*
 * The pin routes these service and failure event types away.
 * They are types 129 to 135 and 176 from enum gdma_eqe_type.
 * The pin has a context callback or device service work for them.
 * This library has neither, so a defined type keeps the quarantine
 * path. Types the pin does not define carry no service meaning.
 * The pin ignores them (gdma_main.c mana_gd_process_eqe default).
 * Do the same here so one benign event cannot poison the context.
 */
static int eq_type_is_service(uint32_t type)
{
	return (type >= 129 && type <= 135) || type == 176;
}

int mana_data_poll(struct mana_data *d)
{
	uint32_t i, entries, info;
	int err = usable(d);
	if (err)
		return err;
	if (!d->eq_live)
		return report(d, "data EQ not live", -ENOTCONN);
	entries = (uint32_t)d->eq.dma.size / 16;
	for (i = 0; i < entries; i++) {
		const unsigned char *entry = (const unsigned char *)d->eq.dma.addr +
			(d->eq_head % entries) * 16;
		struct mana_data_queue *q = NULL;
		unsigned int j;
		err = owner(d, entry + 12, d->eq_head, entries, &info);
		if (err < 0)
			return err;
		if (!err)
			break;
		if (info & UINT32_C(0x1ffcff00))
			return poison(d, "data EQ reserved fields", -EPROTO);
		switch (info & 0xff) {
		case 3:
			for (j = 0; j < 2; j++)
				if (d->queues[j] &&
				    ((d->queues[j]->object.live &&
				      d->queues[j]->object.cq_id ==
				      (mana_get32(entry) & 0xffffff)) ||
				     (d->queues[j]->object_destroyed &&
				      d->queues[j]->retired_cq_id ==
				      (mana_get32(entry) & 0xffffff))))
					q = d->queues[j];
			if (!q)
				return poison(d, "data EQ completion ID", -EPROTO);
			if (!q->object_destroyed)
				q->notified = 1;
			break;
		case 64:
			d->test_seen = 1;
			break;
		default:
			if (eq_type_is_service(info & 0xff))
				return poison(d, "unsupported data EQ event",
					      -ENOTSUP);
			d->unknown_events++;
			break;
		}
		d->eq_head++;
	}
	if (i)
		doorbell(d, 0xff8, d->eq_id, d->eq_head % (entries * 8), 1);
	return 0;
}

static int test_eq(struct mana_data *d)
{
	unsigned char req[44] = { 0 }, resp[32] = { 0 };
	struct mana_bootstrap *b = d->control->bootstrap;
	unsigned int i, before = b->interrupts;
	int err;
	d->test_seen = 0;
	header(req, GENERATE_TEST_EQE, sizeof(req), sizeof(resp), b->mana_device_id);
	mana_put32(req + 40, d->eq_id);
	err = command(d, req, sizeof(req), resp, sizeof(resp));
	if (err)
		return err;
	for (i = 0; i < DATA_TEST_POLLS; i++) {
		err = mana_control_poll(d->control);
		if (!err)
			err = mana_data_poll(d);
		if (err)
			return poison(d, "data EQ test poll", err);
		if (d->test_seen && b->interrupts != before)
			return 0;
		err = b->p->delay_us(b->p->ctx, 1000);
		if (err)
			return poison(d, "data EQ test clock", err);
	}
	return poison(d, "data EQ test timeout", -ETIMEDOUT);
}

int mana_data_start(struct mana_data *d, struct mana_control *c,
		    uint32_t eq_size)
{
	unsigned char req[96] = { 0 }, resp[36] = { 0 };
	struct mana_bootstrap *b;
	int err;
	if (!d || !c || !c->bootstrap)
		return -EINVAL;
	if (d->control || c->data_owned) {
		c->bootstrap->p->error(c->bootstrap->p->ctx,
				       "data already owned", -EBUSY);
		return -EBUSY;
	}
	d->control = c;
	b = c->bootstrap;
	if (!c->ready || c->quarantined || !b->irq_owned ||
	    !valid_size(d, eq_size) || !c->max_eqs || b->limits.eq < 2 ||
	    !b->limits.msix || c->doorbell_id >= b->limits.doorbells) {
		report(d, "data EQ configuration", -EINVAL);
		d->control = NULL;
		return -EINVAL;
	}
	c->data_owned = 1;
	err = region_create(d, &d->eq, eq_size);
	if (err)
		return err;
	header(req, CREATE_QUEUE, sizeof(req), sizeof(resp), b->mana_device_id);
	mana_put32(req + 40, GDMA_EQ);
	mana_put32(req + 48, c->pd_id);
	mana_put32(req + 52, c->doorbell_id);
	mana_put64(req + 56, d->eq.handle);
	mana_put32(req + 68, eq_size);
	mana_put32(req + 72, 1); /* Throttle limit, log2. MSI-X index zero. */
	err = command(d, req, sizeof(req), resp, sizeof(resp));
	if (err)
		return err;
	consumed(&d->eq);
	d->eq_live = 1;
	d->eq_id = mana_get32(resp + 32);
	d->eq_head = eq_size / 16;
	if (d->eq_id > 0xffff || d->eq_id == b->eq_id)
		return poison(d, "data EQ identifier", -EPROTO);
	err = test_eq(d);
	if (!err)
		d->ready = 1;
	return err;
}

int mana_data_queue_create(struct mana_data *d, struct mana_data_queue *q,
			   enum mana_control_queue_type type,
			   uint32_t wq_size, uint32_t cq_size)
{
	struct mana_control_queue_spec spec;
	struct mana_bootstrap *b;
	unsigned int slot;
	int err = usable(d);
	if (err)
		return err;
	b = d->control->bootstrap;
	if (!d->ready || !q || q->data ||
	    (type != MANA_CONTROL_TX && type != MANA_CONTROL_RX) ||
	    !valid_size(d, wq_size) || !valid_size(d, cq_size))
		return report(d, "data queue configuration", -EINVAL);
	slot = type == MANA_CONTROL_TX ? 0 : 1;
	if (d->queues[slot] ||
	    (type == MANA_CONTROL_TX ? b->limits.sq < 2 || !d->control->max_sq :
				     b->limits.rq < 2 || !d->control->max_rq) ||
	    b->limits.cq < 2 + (d->queues[1 - slot] != NULL))
		return report(d, "data queue resource limit", -ENOSPC);
	q->data = d;
	d->queues[slot] = q;
	err = region_create(d, &q->wq, wq_size);
	if (!err)
		err = region_create(d, &q->cq, cq_size);
	if (err)
		return err;
	spec = (struct mana_control_queue_spec) {
		.wq_region = q->wq.handle, .cq_region = q->cq.handle,
		.wq_size = wq_size, .cq_size = cq_size,
		.parent_eq = d->eq_id, .moderation_context = 0
	};
	err = mana_control_create_queue(d->control, &q->object, type, &spec);
	if (err) {
		if (d->control->quarantined)
			return poison(d, "data object create uncertain", err);
		return err;
	}
	consumed(&q->wq);
	consumed(&q->cq);
	q->cq_head = cq_size / 64;
	doorbell(d, 0x800, q->object.cq_id, q->cq_head, 1);
	return 0;
}

int mana_data_queue_quiesce(struct mana_data_queue *q)
{
	int err = usable(q ? q->data : NULL);
	if (err)
		return err;
	q->stopped = 1;
	return 0;
}

int mana_data_queue_destroy(struct mana_data_queue *q)
{
	struct mana_data *d = q ? q->data : NULL;
	unsigned int i;
	int err = usable(d);
	if (err)
		return err;
	if (q->object.live) {
		uint32_t cq_id = q->object.cq_id;
		if (!q->stopped || (q->object.type == MANA_CONTROL_TX &&
				   q->head != q->tail))
			return report(d, "data WQ not drained", -EBUSY);
		err = mana_control_destroy_queue(&q->object);
		if (err) {
			if (d->control->quarantined)
				return poison(d, "data object destroy uncertain", err);
			return err;
		}
		q->wq.consumed = q->cq.consumed = 0;
		q->object_destroyed = 1;
		q->retired_cq_id = cq_id;
	}
	if (q->object_destroyed) {
		/* Flush notifications before removing this CQ's identity. */
		err = test_eq(d);
		if (err)
			return err;
	}
	err = region_release(d, &q->cq);
	if (!err)
		err = region_release(d, &q->wq);
	if (err)
		return err;
	for (i = 0; i < 2; i++)
		if (d->queues[i] == q)
			d->queues[i] = NULL;
	memset(q, 0, sizeof(*q));
	return 0;
}

int mana_data_stop(struct mana_data *d)
{
	unsigned char req[52] = { 0 }, resp[32] = { 0 };
	struct mana_control *c;
	int err = usable(d);
	if (err)
		return err;
	if (d->queues[0] || d->queues[1] || d->control->tx || d->control->rx)
		return report(d, "data objects still owned", -EBUSY);
	c = d->control;
	d->ready = 0;
	if (d->eq_live) {
		err = test_eq(d);
		if (err)
			return err;
		header(req, DISABLE_QUEUE, sizeof(req), sizeof(resp),
		       c->bootstrap->mana_device_id);
		mana_put32(req + 40, GDMA_EQ);
		mana_put32(req + 44, d->eq_id);
		mana_put32(req + 48, 1);
		err = command(d, req, sizeof(req), resp, sizeof(resp));
		if (err)
			return err;
		d->eq_live = 0;
		d->eq.consumed = 0;
	}
	err = region_release(d, &d->eq);
	if (err)
		return err;
	if (d->regions)
		return report(d, "data region accounting", -EBUSY);
	c->data_owned = 0;
	memset(d, 0, sizeof(*d));
	return 0;
}

int mana_data_post(struct mana_data_queue *q, const struct mana_data_work *work,
		   struct mana_data_posted *posted)
{
	unsigned char wqe[512] = { 0 };
	struct mana_data *d = q ? q->data : NULL;
	uint32_t size, oob_size, i, offset, first, used;
	unsigned int is_sq;
	int err = usable(d);
	if (err)
		return err;
	is_sq = q->object.type == MANA_CONTROL_TX;
	if (!q->object.live || q->object.quarantined || q->stopped ||
	    !work || !posted || !work->sges || !work->num_sge ||
	    work->num_sge > MANA_DATA_MAX_SGE || work->data_unit > 0x3fff ||
	    (is_sq ? (work->oob_size != 8 && work->oob_size != 24) ||
		     !work->oob : work->oob_size || work->data_unit))
		return report(d, "data WQE configuration", -EINVAL);
	oob_size = is_sq ? work->oob_size : 8;
	size = (8 + oob_size + work->num_sge * 16 + 31) & ~31U;
	if (size > (is_sq ? 512U : 256U))
		return report(d, "data WQE size", -E2BIG);
	used = q->head - q->tail;
	if (used > q->wq.dma.size / 32)
		return poison(d, "data WQ accounting", -EOVERFLOW);
	if (size / 32 > q->wq.dma.size / 32 - used)
		return report(d, "data WQ full", -ENOSPC);
	mana_put32(wqe + 4, work->num_sge | (oob_size / 4) << 8 |
		   work->data_unit << 16);
	if (is_sq)
		memcpy(wqe + 8, work->oob, oob_size);
	for (i = 0; i < work->num_sge; i++) {
		const struct mana_data_sge *sge = &work->sges[i];
		unsigned char *out = wqe + 8 + oob_size + i * 16;
		if (!sge->size || sge->address > UINT64_MAX - (sge->size - 1) ||
		    sge->memory_key != d->control->mkey)
			return report(d, "data SGE bounds or key", -EINVAL);
		mana_put64(out, sge->address);
		mana_put32(out + 8, sge->memory_key);
		mana_put32(out + 12, sge->size);
	}
	offset = (q->head * 32) & ((uint32_t)q->wq.dma.size - 1);
	first = (uint32_t)q->wq.dma.size - offset;
	if (first > size)
		first = size;
	memcpy((unsigned char *)q->wq.dma.addr + offset, wqe, first);
	memcpy(q->wq.dma.addr, wqe + first, size - first);
	posted->offset = offset;
	posted->basic_units = size / 32;
	q->head += size / 32;
	doorbell(d, is_sq ? 0 : 0x400, q->object.wq_id, q->head * 32, 0);
	return 0;
}

int mana_data_retire(struct mana_data_queue *q,
		     const struct mana_data_posted *posted)
{
	struct mana_data *d = q ? q->data : NULL;
	int err = usable(d);
	if (err)
		return err;
	if (!q->object.live || !posted || !posted->basic_units ||
	    posted->basic_units > (q->object.type == MANA_CONTROL_TX ? 16U : 8U) ||
	    posted->basic_units > q->head - q->tail ||
	    posted->offset != ((q->tail * 32) & ((uint32_t)q->wq.dma.size - 1)))
		return report(d, "data WQE retirement", -ERANGE);
	q->tail += posted->basic_units;
	return 0;
}

int mana_data_poll_cq(struct mana_data_queue *q,
		      struct mana_data_completion *completion)
{
	struct mana_data *d = q ? q->data : NULL;
	const unsigned char *entry;
	uint32_t entries, info, cqe_header;
	int err = usable(d);
	if (err)
		return err;
	if (!completion || !q->object.live || q->object.quarantined)
		return report(d, "data CQ state", -EINVAL);
	entries = (uint32_t)q->cq.dma.size / 64;
	entry = (const unsigned char *)q->cq.dma.addr +
		(q->cq_head % entries) * 64;
	err = owner(d, entry + 60, q->cq_head, entries, &info);
	if (err <= 0) {
		if (!err)
			q->notified = 0;
		return err;
	}
	if ((info & 0xffffff) != q->object.wq_id || (info & 0x1e000000) ||
	    !!(info & (1U << 24)) != (q->object.type == MANA_CONTROL_TX))
		return poison(d, "data CQ WQ identifier", -EPROTO);
	cqe_header = mana_get32(entry);
	if ((cqe_header & 0xc0) != 0x40)
		return poison(d, "data CQ client type", -EPROTO);
	completion->kind = MANA_DATA_COMPLETION;
	if ((cqe_header & 0x3f) == 3) {
		if (cqe_header != 0x43 || q->object.type != MANA_CONTROL_RX ||
		    !q->object.fence_pending)
			return poison(d, "data RX fence completion", -EPROTO);
		err = mana_control_fence_complete(&q->object);
		if (err)
			return poison(d, "data RX fence state", err);
		completion->kind = MANA_DATA_RX_FENCE;
	}
	completion->wq_id = info & 0xffffff;
	completion->is_sq = !!(info & (1U << 24));
	memcpy(completion->data, entry, sizeof(completion->data));
	q->cq_head++;
	doorbell(d, 0x800, q->object.cq_id, q->cq_head % (entries * 8), 1);
	return 1;
}
