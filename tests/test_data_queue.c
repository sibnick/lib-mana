/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include <uk/mana/data_queue.h>
#include <stdlib.h>

struct registered_region {
	uint64_t handle, address;
	uint32_t size;
	unsigned int live, consumed;
};

struct fixture {
	struct host_bootstrap model;
	struct mana_control control;
	struct mana_data data;
	struct mana_data_queue tx, rx;
	struct registered_region regions[8];
	uint32_t region_count, eq_producer, eq_id;
	uint32_t status_command, mutate_command, timeout_command;
	uint32_t timeout_region, timeout_destroy;
	unsigned int short_response, bad_id, zero_handle, duplicate_handle;
	unsigned int no_test, commands, mismatch, region_destroys;
	unsigned int object_destroys, eq_disables, eq_bells, cq_bells, wq_bells;
	unsigned int reads, writes, consumed_destroys;
	void (*write64)(void *, const struct mana_mapping *, uint64_t, uint64_t);
};

static void eq_event(struct fixture *f, uint32_t type, uint32_t id)
{
	uint32_t entries = (uint32_t)f->data.eq.dma.size / 16;
	unsigned char *entry = (unsigned char *)f->data.eq.dma.addr +
		(f->eq_producer % entries) * 16;
	memset(entry, 0, 16);
	mana_put32(entry, id);
	mana_put32(entry + 12, type | ((f->eq_producer / entries) & 7) << 29);
	f->eq_producer++;
}

static void expected_header(unsigned char *out, uint32_t code,
			    uint32_t size, uint32_t response_size,
			    uint32_t device)
{
	mana_put32(out + 4, code);
	mana_put16(out + 8, 1);
	mana_put32(out + 12, size);
	mana_put32(out + 20, code);
	mana_put16(out + 24, 1);
	mana_put32(out + 28, response_size);
	mana_put32(out + 32, device);
}

static struct registered_region *find_region(struct fixture *f, uint64_t handle)
{
	uint32_t i;
	for (i = 0; i < f->region_count; i++)
		if (f->regions[i].handle == handle && f->regions[i].live)
			return &f->regions[i];
	return NULL;
}

static int respond(void *ctx, const unsigned char *req, size_t size,
		   unsigned char *resp, size_t capacity, size_t *length)
{
	struct fixture *f = ctx;
	unsigned char expected[4096] = { 0 };
	uint32_t code = mana_get32(req + 4), pages, i, type;
	uint32_t in_size = 0, out_size = 0, device = 0x10002;
	struct registered_region *r;
	int failed;
	if (code == 1 || code == 2 || code == 3 ||
	    (code == 10 && mana_get32(req + 32) == 1))
		return -ENOTSUP;
	f->commands++;
	/* A firmware status and a truncated response both fail a command
	 * deterministically. The pin releases ownership on both paths.
	 */
	failed = code == f->status_command ||
		 (f->short_response && code == f->mutate_command);
	if (code == f->timeout_command &&
	    (code != 25 || !f->timeout_region ||
	     f->region_count + 1 == f->timeout_region) &&
	    (code != 27 || !f->timeout_destroy ||
	     f->region_destroys + 1 == f->timeout_destroy))
		f->model.fault = HOST_BOOTSTRAP_NO_TX;
	if (code == 25 || code == 27 || code == 12 || code == 13 || code == 10) {
		switch (code) {
		case 25:
			device = 0;
			pages = (uint32_t)mana_get64(req + 40) / 4096;
			in_size = 64 + pages * 8;
			out_size = 40;
			r = &f->regions[f->region_count];
			r->handle = 0x12340000 + f->region_count + 1;
			r->address = mana_get64(req + 64);
			r->size = pages * 4096;
			r->live = !failed;
			if (f->zero_handle)
				r->handle = 0;
			if (f->duplicate_handle && f->region_count == 2)
				r->handle = f->regions[1].handle;
			f->region_count++;
			mana_put64(expected + 40, r->size);
			mana_put32(expected + 56, pages);
			mana_put32(expected + 60, pages);
			for (i = 0; i < pages; i++)
				mana_put64(expected + 64 + i * 8, r->address +
					   (uint64_t)i * 4096);
			mana_put64(resp + 32, r->handle);
			for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++)
				if (f->model.host.dma[i].device_address == r->address &&
				    f->model.host.dma[i].size == r->size)
					break;
			if (i == HOST_MOCK_DMA_SLOTS)
				f->mismatch = 1;
			break;
		case 27:
			device = 0;
			in_size = 48;
			out_size = 32;
			r = find_region(f, mana_get64(req + 40));
			if (!r)
				f->mismatch = 1;
			else {
				if (r->consumed)
					f->consumed_destroys++;
				if (!failed)
					r->live = 0;
			}
			mana_put64(expected + 40, mana_get64(req + 40));
			f->region_destroys++;
			break;
		case 12:
			in_size = 96;
			out_size = 36;
			r = find_region(f, mana_get64(req + 56));
			if (!r || r->consumed)
				f->mismatch = 1;
			else if (!failed)
				r->consumed = 1;
			mana_put32(expected + 40, 4);
			mana_put32(expected + 48, 23);
			mana_put32(expected + 52, 0);
			mana_put64(expected + 56, f->regions[0].handle);
			mana_put32(expected + 68, f->regions[0].size);
			mana_put32(expected + 72, 1);
			f->eq_id = 17;
			f->eq_producer = f->regions[0].size / 16;
			mana_put32(resp + 32, f->bad_id ? 0x10000 : f->eq_id);
			break;
		case 13:
			in_size = 52;
			out_size = 32;
			mana_put32(expected + 40, 4);
			mana_put32(expected + 44, 17);
			mana_put32(expected + 48, 1);
			if (!failed) {
				f->regions[0].live = 0;
				f->eq_disables++;
			}
			break;
		case 10:
			in_size = 44;
			out_size = 32;
			mana_put32(expected + 40, 17);
			if (!f->no_test && !failed)
				eq_event(f, 64, 0);
			break;
		}
		expected_header(expected, code, in_size, out_size, device);
		if (size != in_size || capacity < out_size ||
		    memcmp(expected, req, size))
			f->mismatch = 1;
	} else {
		/* MANA responses use the independent control suite's wire shapes. */
		switch (code) {
		case 4:
			mana_put32(resp + 32, 23);
			mana_put32(resp + 36, 0x91);
			break;
		case 5:
			break;
		case 0x20001:
			mana_put16(resp + 64, 1);
			mana_put32(resp + 68, 4);
			mana_put16(resp + 72, 9014);
			break;
		case 0x20008:
			mana_put32(resp + 32, 2);
			mana_put32(resp + 36, 2);
			mana_put32(resp + 40, 64);
			memcpy(resp + 48, "\x02\x11\x22\x33\x44\x55", 6);
			mana_put64(resp + 56, 0x12345678);
			break;
		case 0x20003:
		case 0x20007:
		case 0x2000a:
		case 0x20006:
			break;
		case 0x20004:
			type = mana_get32(req + 48);
			for (i = 0; i < 2; i++) {
				r = find_region(f, mana_get64(req + 56 + i * 8));
				if (!r || r->consumed ||
				    mana_get32(req + 72 + i * 4) != r->size)
					f->mismatch = 1;
				else if (!failed)
					r->consumed = 1;
			}
			if (mana_get32(req + 84) != 17 ||
			    mana_get32(req + 80) || mana_get32(req + 88) ||
			    mana_get32(req + 92))
				f->mismatch = 1;
			mana_put32(resp + 32, type == MANA_CONTROL_TX ? 21 : 22);
			mana_put32(resp + 36, type == MANA_CONTROL_TX ? 10 : 11);
			mana_put64(resp + 40, 0x56780000 + type);
			if (f->bad_id)
				mana_put32(resp + 36, 5);
			break;
		case 0x20005:
			f->object_destroys++;
			break;
		default:
			abort();
		}
	}
	if (code == f->status_command)
		mana_put32(resp + 24, 1);
	if (code == f->mutate_command && f->short_response)
		*length = 32;
	return 0;
}

static void write64(void *ctx, const struct mana_mapping *m,
		    uint64_t off, uint64_t value)
{
	struct fixture *f = ctx;
	uint32_t id = (uint32_t)value & 0xffffff;
	if ((off == 0x1000 && id == 7) ||
	    (off == 0x1400 && id == 6) ||
	    (off == 0x1800 && id == 5) ||
	    (off == 0x1ff8 && id == 9)) {
		f->write64(ctx, m, off, value);
		return;
	}
	if (host_mock_write(&f->model.host, (size_t)off, 8, value))
		abort();
	if (off == 0x1ff8 && id == 17) {
		f->eq_bells++;
		if (!(value >> 63) || (uint32_t)(value >> 32) !=
		    (0x80000000U | (f->data.eq_head %
				    ((uint32_t)f->data.eq.dma.size / 2))))
			f->mismatch = 1;
	} else if (off == 0x1800 && (id == 10 || id == 11)) {
		struct mana_data_queue *q = id == 10 ? &f->tx : &f->rx;
		f->cq_bells++;
		if (!(value >> 63) || ((uint32_t)value & 0xff000000) ||
		    (uint32_t)(value >> 32) !=
		    (0x80000000U | (q->cq_head %
				    ((uint32_t)q->cq.dma.size / 8))))
			f->mismatch = 1;
	} else if ((off == 0x1000 && id == 21) ||
		   (off == 0x1400 && id == 22)) {
		struct mana_data_queue *q = id == 21 ? &f->tx : &f->rx;
		f->wq_bells++;
		if (((uint32_t)value & 0xff000000) ||
		    (uint32_t)(value >> 32) != q->head * 32)
			f->mismatch = 1;
	} else {
		f->mismatch = 1;
	}
}

static void rmb(void *ctx)
{
	struct fixture *f = ctx;
	f->reads++;
}

static void wmb(void *ctx)
{
	struct fixture *f = ctx;
	f->writes++;
}

static void init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	f->model.respond = respond;
	f->model.respond_ctx = f;
	f->write64 = f->model.platform.write64;
	f->model.platform.write64 = write64;
	f->model.platform.dma_rmb = rmb;
	f->model.platform.dma_wmb = wmb;
}

static int start_control(struct fixture *f)
{
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_control_start(&f->control, &f->model.b, 0) == 0);
	return 0;
}

static int start(struct fixture *f)
{
	init(f);
	TEST_CHECK(start_control(f) == 0);
	TEST_CHECK(mana_data_start(&f->data, &f->control, 4096) == 0);
	TEST_CHECK(f->data.ready && f->data.regions == 1 && f->control.data_owned);
	TEST_CHECK(!f->data.eq.handle && f->data.eq.consumed);
	return 0;
}

static int stop_control(struct fixture *f)
{
	TEST_CHECK(mana_control_stop(&f->control) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	TEST_CHECK(!f->model.mismatch && !f->mismatch && !f->consumed_destroys);
	return 0;
}

static int stop(struct fixture *f)
{
	TEST_CHECK(mana_data_stop(&f->data) == 0);
	TEST_CHECK(!f->data.control && !f->control.data_owned);
	TEST_CHECK(f->eq_disables == 1);
	return stop_control(f);
}

static int discard(struct fixture *f, size_t allocations)
{
	TEST_CHECK(f->data.quarantined && f->control.quarantined);
	TEST_CHECK(mana_data_stop(&f->data) == -ENOTCONN);
	TEST_CHECK(mana_control_stop(&f->control) == -ENOTCONN);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	TEST_CHECK(f->model.host.live_allocations == allocations);
	TEST_CHECK(!f->mismatch && !f->model.mismatch && !f->consumed_destroys);
	/* Dispose only the simulated device, not production quarantine. */
	TEST_CHECK(host_mock_cleanup(&f->model.host) == allocations);
	return 0;
}

static void cqe(struct mana_data_queue *q, uint32_t header, uint32_t info)
{
	uint32_t entries = (uint32_t)q->cq.dma.size / 64;
	unsigned char *entry = (unsigned char *)q->cq.dma.addr +
		(q->cq_head % entries) * 64;
	memset(entry, 0, 64);
	mana_put32(entry, header);
	mana_put32(entry + 56, 0xabcdef98);
	mana_put32(entry + 60, info | ((q->cq_head / entries) & 7) << 29);
}

static int lifecycle(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_control_stop(&f.control) == -EBUSY);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  8192, 4096) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
					  4096, 8192) == 0);
	TEST_CHECK(f.data.regions == 5 && f.model.host.live_allocations == 11);
	TEST_CHECK(!f.tx.wq.handle && !f.tx.cq.handle);
	TEST_CHECK(f.tx.wq.consumed && f.tx.cq.consumed);
	TEST_CHECK(mana_data_stop(&f.data) == -EBUSY);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == -EBUSY);
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == 0);
	TEST_CHECK(mana_control_set_rx(&f.control, 0) == 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.rx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == -EBUSY);
	TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
	TEST_CHECK(!f.rx.object.fenced && f.rx.object.fence_pending);
	TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 0);
	cqe(&f.rx, 0x43, 22);
	TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 1);
	TEST_CHECK(completion.kind == MANA_DATA_RX_FENCE && !completion.is_sq);
	TEST_CHECK(f.rx.object.fenced && !f.rx.object.fence_pending);
	eq_event(&f, 3, 11);
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	eq_event(&f, 3, 10);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	TEST_CHECK(f.object_destroys == 2 && f.region_destroys == 0);
	TEST_CHECK(f.model.host.live_allocations == 7 && f.data.regions == 1);
	return stop(&f);
}

static int owner_wrap(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	uint32_t i;
	unsigned int reads;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	reads = f.reads;
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 0);
	TEST_CHECK(f.reads == reads);
	for (i = 0; i < 256 * 9; i++) {
		eq_event(&f, 3, 10);
		TEST_CHECK(mana_data_poll(&f.data) == 0 && f.tx.notified);
		cqe(&f.tx, 0x60, 21 | (1U << 24));
		TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 1);
		TEST_CHECK(completion.wq_id == 21 && completion.is_sq);
		TEST_CHECK(completion.kind == MANA_DATA_COMPLETION);
		TEST_CHECK(mana_get32(completion.data + 56) == 0xabcdef98);
		TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 0);
		TEST_CHECK(!f.tx.notified);
	}
	TEST_CHECK(f.cq_bells == 1 + 256 * 9);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	return stop(&f);
}

static int work_rings(void)
{
	struct fixture f;
	struct mana_data_sge sges[30];
	struct mana_data_posted posted, oldest;
	unsigned char oob[24], expected[512];
	struct mana_data_work work = {
		.sges = sges, .num_sge = 1, .data_unit = 0x1234,
		.oob = oob, .oob_size = 8
	};
	uint32_t i, j;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
					  4096, 4096) == 0);
	memset(oob, 0xc5, sizeof(oob));
	for (i = 0; i < 30; i++)
		sges[i] = (struct mana_data_sge) {
			.address = UINT64_C(0x1234567800000000) + i * 4096,
			.memory_key = 0x91, .size = 1500
		};
	for (i = 0; i < 127; i++) {
		TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == 0);
		TEST_CHECK(posted.offset == i * 32 && posted.basic_units == 1);
		TEST_CHECK(mana_data_retire(&f.tx, &posted) == 0);
	}
	work.oob_size = 24;
	work.num_sge = 30;
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == 0);
	TEST_CHECK(posted.offset == 4064 && posted.basic_units == 16);
	memset(expected, 0, sizeof(expected));
	mana_put32(expected + 4, 30 | (6U << 8) | (0x1234U << 16));
	memcpy(expected + 8, oob, 24);
	for (i = 0; i < 30; i++) {
		mana_put64(expected + 32 + i * 16, sges[i].address);
		mana_put32(expected + 40 + i * 16, 0x91);
		mana_put32(expected + 44 + i * 16, 1500);
	}
	TEST_CHECK(!memcmp((unsigned char *)f.tx.wq.dma.addr + 4064, expected, 32));
	TEST_CHECK(!memcmp(f.tx.wq.dma.addr, expected + 32, 480));
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == -EBUSY);
	TEST_CHECK(mana_data_retire(&f.tx, &posted) == 0);
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	work.oob = NULL;
	work.oob_size = work.data_unit = 0;
	work.num_sge = 15;
	TEST_CHECK(mana_data_post(&f.rx, &work, &oldest) == 0);
	TEST_CHECK(oldest.basic_units == 8);
	for (i = 1; i < 16; i++)
		TEST_CHECK(mana_data_post(&f.rx, &work, &posted) == 0);
	TEST_CHECK(mana_data_post(&f.rx, &work, &posted) == -ENOSPC);
	TEST_CHECK(f.rx.head == 128 && f.rx.tail == 0);
	posted = oldest;
	posted.offset = 32;
	TEST_CHECK(mana_data_retire(&f.rx, &posted) == -ERANGE);
	TEST_CHECK(f.rx.tail == 0);
	for (j = 0; j < 16; j++) {
		posted.offset = j * 256;
		posted.basic_units = 8;
		TEST_CHECK(mana_data_retire(&f.rx, &posted) == 0);
	}
	TEST_CHECK(mana_data_queue_quiesce(&f.rx) == 0);
	TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
	cqe(&f.rx, 0x43, 22);
	{
		struct mana_data_completion completion;
		TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 1);
	}
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
	TEST_CHECK(f.wq_bells == 127 + 1 + 16);
	return stop(&f);
}

/* The RQ long-form WQE accepts at most 15 SGEs inside the 256-byte limit. */
static int rq_sge_limit(void)
{
	struct fixture f;
	struct mana_data_sge sges[16];
	struct mana_data_posted posted;
	struct mana_data_work work = { .sges = sges, .num_sge = 16 };
	uint32_t i;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
					  4096, 4096) == 0);
	for (i = 0; i < 16; i++)
		sges[i] = (struct mana_data_sge) {
			.address = UINT64_C(0x1234567800000000) + i * 4096,
			.memory_key = 0x91, .size = 1500
		};
	/* 8 header + 8 OOB + 16 SGEs need 272 bytes of the 256-byte WQE. */
	TEST_CHECK(mana_data_post(&f.rx, &work, &posted) == -E2BIG);
	TEST_CHECK(f.rx.head == 0 && !f.wq_bells);
	work.num_sge = 15;
	TEST_CHECK(mana_data_post(&f.rx, &work, &posted) == 0);
	TEST_CHECK(posted.basic_units == 8 && f.rx.head == 8 && f.wq_bells == 1);
	TEST_CHECK(mana_data_retire(&f.rx, &posted) == 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.rx) == 0);
	/* RX destroy needs a validated fence like every RX queue. */
	TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
	cqe(&f.rx, 0x43, 22);
	{
		struct mana_data_completion completion;
		TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 1);
		TEST_CHECK(completion.kind == MANA_DATA_RX_FENCE);
	}
	eq_event(&f, 3, 11);
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
	return stop(&f);
}

/* The post precheck counts full WQE units. A long-form 2-unit WQE must
 * wait for two free units, not one.
 */
static int long_form_capacity(void)
{
	struct fixture f;
	struct mana_data_sge sge = { 4096, 0x91, 1500 };
	unsigned char oob[24] = { 0 };
	struct mana_data_work long_work = { &sge, 1, 0x2a, oob, 24 };
	struct mana_data_work short_work = { &sge, 1, 0x2a, oob, 8 };
	struct mana_data_posted posted, longs[63], spare, wrapped;
	uint32_t i, bells;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	for (i = 0; i < 63; i++) {
		TEST_CHECK(mana_data_post(&f.tx, &long_work, &posted) == 0);
		TEST_CHECK(posted.basic_units == 2 && posted.offset == i * 64);
		longs[i] = posted;
	}
	/* Fill 126 units with long-form WQEs, then 1 unit with a short one. */
	TEST_CHECK(mana_data_post(&f.tx, &short_work, &spare) == 0);
	TEST_CHECK(spare.basic_units == 1 && f.tx.head == 127);
	bells = f.wq_bells;
	/* One free unit cannot hold a 2-unit WQE. */
	TEST_CHECK(mana_data_post(&f.tx, &long_work, &posted) == -ENOSPC);
	TEST_CHECK(f.tx.head == 127 && f.tx.tail == 0);
	TEST_CHECK(f.wq_bells == bells);
	/* Retiring the oldest 2-unit WQE frees room for one long post. */
	TEST_CHECK(mana_data_retire(&f.tx, &longs[0]) == 0);
	TEST_CHECK(mana_data_post(&f.tx, &long_work, &wrapped) == 0);
	TEST_CHECK(wrapped.offset == 4064 && wrapped.basic_units == 2);
	TEST_CHECK(f.tx.head == 129 && f.wq_bells == bells + 1);
	/* Drain FIFO so destroy sees a fully retired ring. */
	for (i = 1; i < 63; i++)
		TEST_CHECK(mana_data_retire(&f.tx, &longs[i]) == 0);
	TEST_CHECK(mana_data_retire(&f.tx, &spare) == 0);
	TEST_CHECK(mana_data_retire(&f.tx, &wrapped) == 0);
	TEST_CHECK(f.tx.head == f.tx.tail);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	return stop(&f);
}
static int input_bounds(void)
{
	struct fixture f;
	struct mana_data_posted posted;
	struct mana_data_sge sge = { UINT64_MAX, 0x91, 2 };
	unsigned char oob[24] = { 0 };
	struct mana_data_work work = { &sge, 1, 0, oob, 8 };
	unsigned int commands;
	init(&f);
	TEST_CHECK(start_control(&f) == 0);
	commands = f.commands;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 0) == -EINVAL);
	TEST_CHECK(mana_data_start(&f.data, &f.control, 6144) == -EINVAL);
	TEST_CHECK(mana_data_start(&f.data, &f.control, 2097152) == -EINVAL);
	f.model.b.limits.eq = 1;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EINVAL);
	TEST_CHECK(f.commands == commands && !f.control.data_owned);
	f.model.b.limits.eq = 32;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EBUSY);
	f.model.b.limits.sq = 1;
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == -ENOSPC);
	f.model.b.limits.sq = 32;
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	sge.address = 4096;
	sge.memory_key = 8;
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	sge.memory_key = 0x91;
	work.num_sge = 31;
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	work.num_sge = 1;
	work.oob_size = 12;
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	work.oob_size = 8;
	work.data_unit = 0x4000;
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == -EINVAL);
	TEST_CHECK(f.tx.head == 0 && !f.wq_bells);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	return stop(&f);
}

static int allocation_failures(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 1; i <= 5; i++) {
		init(&f);
		TEST_CHECK(start_control(&f) == 0);
		TEST_CHECK(host_fault_arm(&f.model.host, HOST_FAULT_ALLOC,
					  i, -ENOMEM) == 0);
		if (i == 1) {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -ENOMEM);
			TEST_CHECK(mana_data_stop(&f.data) == 0);
		} else if (i <= 3) {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == -ENOMEM);
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
			TEST_CHECK(mana_data_stop(&f.data) == 0);
		} else {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == 0);
			TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
							  4096, 4096) == -ENOMEM);
			TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
			TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
			TEST_CHECK(mana_data_stop(&f.data) == 0);
		}
		TEST_CHECK(stop_control(&f) == 0);
	}
	init(&f);
	TEST_CHECK(start_control(&f) == 0);
	f.model.dma_bad_at = 7;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EPROTO);
	TEST_CHECK(mana_data_stop(&f.data) == 0);
	return stop_control(&f);
}

static int large_ring_boundary(void)
{
	struct fixture f;
	init(&f);
	TEST_CHECK(start_control(&f) == 0);
	TEST_CHECK(mana_data_start(&f.data, &f.control, 1048576) == 0);
	TEST_CHECK(f.regions[0].size == 1048576);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  1048576, 4096) == 0);
	TEST_CHECK(f.regions[1].size == 1048576);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	return stop(&f);
}

static int firmware_failures(void)
{
	struct fixture f;
	unsigned int i;
	static const uint32_t codes[] = { 25, 12, 0x20004 };
	for (i = 0; i < 3; i++) {
		init(&f);
		TEST_CHECK(start_control(&f) == 0);
		f.status_command = codes[i];
		if (codes[i] != 0x20004) {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EPROTO);
			TEST_CHECK(!f.data.quarantined);
			f.status_command = 0;
			TEST_CHECK(mana_data_stop(&f.data) == 0);
		} else {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == -EPROTO);
			TEST_CHECK(f.tx.wq.registered && f.tx.cq.registered);
			f.status_command = 27;
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == -EPROTO);
			TEST_CHECK(f.model.host.live_allocations == 9);
			f.status_command = 0;
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
			TEST_CHECK(f.region_destroys == 3);
			f.status_command = 13;
			TEST_CHECK(mana_data_stop(&f.data) == -EPROTO);
			TEST_CHECK(f.data.eq_live && !f.data.ready);
			TEST_CHECK(f.model.host.live_allocations == 7);
			f.status_command = 0;
			TEST_CHECK(mana_data_stop(&f.data) == 0);
		}
		TEST_CHECK(stop_control(&f) == 0);
	}
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	f.status_command = 0x20005;
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == -EPROTO);
	TEST_CHECK(f.tx.object.live && f.tx.wq.consumed && f.tx.cq.consumed);
	f.status_command = 0;
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	TEST_CHECK(!f.region_destroys);
	return stop(&f);
}

static int uncertain_create(void)
{
	struct fixture f;
	unsigned int i;
	static const uint32_t codes[] = { 25, 12, 0x20004 };
	for (i = 0; i < 3; i++) {
		init(&f);
		TEST_CHECK(start_control(&f) == 0);
		if (codes[i] == 0x20004)
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
		f.timeout_command = codes[i];
		if (codes[i] == 0x20004) {
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == -ETIMEDOUT);
			TEST_CHECK(f.tx.wq.quarantined && f.tx.cq.quarantined);
			TEST_CHECK(f.tx.wq.handle && f.tx.cq.handle);
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == -ENOTCONN);
			TEST_CHECK(discard(&f, 9) == 0);
		} else {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -ETIMEDOUT);
			TEST_CHECK(discard(&f, 7) == 0);
		}
		TEST_CHECK(!f.region_destroys && !f.eq_disables && !f.object_destroys);
	}
	for (i = 0; i < 3; i++) {
		init(&f);
		TEST_CHECK(start_control(&f) == 0);
		if (codes[i] == 0x20004)
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
		f.mutate_command = codes[i];
		f.short_response = 1;
		if (codes[i] == 0x20004) {
			/* A short MANA create response fails the command
			 * only. Both regions return for proven cleanup.
			 */
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == -EPROTO);
			TEST_CHECK(!f.data.quarantined && !f.control.quarantined);
			TEST_CHECK(!f.tx.wq.quarantined && !f.tx.cq.quarantined);
			TEST_CHECK(f.tx.wq.registered && f.tx.cq.registered);
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
			TEST_CHECK(f.region_destroys == 2);
			TEST_CHECK(!f.eq_disables && !f.object_destroys);
		} else {
			/* A short GDMA response fails the command only. */
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EPROTO);
			TEST_CHECK(!f.data.quarantined && !f.control.quarantined);
			TEST_CHECK(mana_data_stop(&f.data) == 0);
			TEST_CHECK(f.region_destroys == (codes[i] == 12 ? 1 : 0));
			TEST_CHECK(!f.eq_disables && !f.object_destroys);
		}
		f.mutate_command = 0;
		f.short_response = 0;
		if (codes[i] == 0x20004)
			TEST_CHECK(stop(&f) == 0);
		else
			TEST_CHECK(stop_control(&f) == 0);
	}
	for (i = 2; i <= 3; i++) {
		TEST_CHECK(start(&f) == 0);
		f.timeout_command = 25;
		f.timeout_region = i;
		TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
						  4096, 4096) == -ETIMEDOUT);
		TEST_CHECK(f.tx.wq.quarantined && f.tx.cq.quarantined);
		TEST_CHECK(discard(&f, i + 6) == 0);
		TEST_CHECK(!f.region_destroys && !f.object_destroys);
	}
	return 0;
}

static int uncertain_cleanup(void)
{
	struct fixture f;
	unsigned int i;
	static const uint32_t codes[] = { 13, 0x20005, 27 };
	for (i = 0; i < 3; i++) {
		TEST_CHECK(start(&f) == 0);
		if (codes[i] != 13) {
			if (codes[i] == 27)
				f.status_command = 0x20004;
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) ==
				   (codes[i] == 27 ? -EPROTO : 0));
			f.status_command = 0;
			TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
		}
		f.timeout_command = codes[i];
		if (codes[i] == 13) {
			TEST_CHECK(mana_data_stop(&f.data) == -ETIMEDOUT);
			TEST_CHECK(discard(&f, 7) == 0);
		} else {
			TEST_CHECK(mana_data_queue_destroy(&f.tx) == -ETIMEDOUT);
			TEST_CHECK(discard(&f, 9) == 0);
		}
	}
	TEST_CHECK(start(&f) == 0);
	f.status_command = 0x20004;
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == -EPROTO);
	f.status_command = 0;
	f.timeout_command = 27;
	f.timeout_destroy = 2;
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == -ETIMEDOUT);
	TEST_CHECK(!f.tx.cq.dma.addr && f.tx.wq.dma.addr);
	TEST_CHECK(f.data.regions == 2);
	TEST_CHECK(discard(&f, 8) == 0);
	return 0;
}

static int invalid_resources(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 0; i < 4; i++) {
		init(&f);
		TEST_CHECK(start_control(&f) == 0);
		if (i == 0) {
			f.zero_handle = 1;
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EPROTO);
			TEST_CHECK(discard(&f, 7) == 0);
		} else if (i == 1) {
			f.bad_id = 1;
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EPROTO);
			TEST_CHECK(discard(&f, 7) == 0);
		} else {
			TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == 0);
			f.bad_id = i == 2;
			f.duplicate_handle = i == 3;
			TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
							  4096, 4096) == -EPROTO);
			TEST_CHECK(discard(&f, 9) == 0);
		}
	}
	return 0;
}

static int bad_events(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	unsigned int i;
	for (i = 0; i < 10; i++) {
		TEST_CHECK(start(&f) == 0);
		TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
						  4096, 4096) == 0);
		if (i == 0) {
			eq_event(&f, 3, 10);
			TEST_CHECK(mana_data_poll(&f.data) == -EPROTO);
		} else if (i == 1) {
			eq_event(&f, 135, 0);
			TEST_CHECK(mana_data_poll(&f.data) == -ENOTSUP);
		} else if (i == 2) {
			eq_event(&f, 3, 11);
			{
				unsigned char *entry = (unsigned char *)f.data.eq.dma.addr +
					(f.data.eq_head % 256) * 16;
				mana_put32(entry + 12, 3 | (3U << 29));
			}
			TEST_CHECK(mana_data_poll(&f.data) == -EOVERFLOW);
		} else {
			uint32_t header = 0x41, info = 22;
			if (i == 3)
				info = 21;
			if (i == 4)
				info |= 1U << 24;
			if (i == 5)
				header = 1;
			if (i == 6)
				header = 0x43;
			if (i == 7) {
				TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
				header = 0x143;
			}
			cqe(&f.rx, header, info);
			if (i == 8) {
				unsigned char *entry = f.rx.cq.dma.addr;
				mana_put32(entry + 60, 22 | (3U << 29));
			}
			if (i == 9) {
				unsigned char *entry = f.rx.cq.dma.addr;
				mana_put32(entry + 60, 22 | (1U << 25) | (1U << 29));
			}
			TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) ==
				   (i == 8 ? -EOVERFLOW : -EPROTO));
			TEST_CHECK(!f.rx.object.fenced);
		}
		TEST_CHECK(discard(&f, 9) == 0);
	}
	return 0;
}

static int unknown_eq_events(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	struct mana_data_sge sge = { 4096, 0x91, 1500 };
	unsigned char oob[8] = { 0 };
	struct mana_data_work work = { &sge, 1, 0x2a, oob, 8 };
	struct mana_data_posted posted;
	static const uint32_t types[] = {
		1, 2, 5, 63, 65, 100, 128, 136, 175, 177, 200, 255
	};
	uint32_t i;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
					  4096, 4096) == 0);
	for (i = 0; i < 12; i++) {
		eq_event(&f, types[i], 0);
		eq_event(&f, 3, i & 1 ? 11 : 10);
		TEST_CHECK(mana_data_poll(&f.data) == 0);
		TEST_CHECK(f.data.unknown_events == i + 1);
		TEST_CHECK(!f.data.quarantined && !f.control.quarantined);
		TEST_CHECK(f.data.ready);
		TEST_CHECK(i & 1 ? f.rx.notified : f.tx.notified);
	}
	/* Full EQ rings of undefined events drain across the owner wrap.
	 * Each batch stays inside the 256-entry ring capacity.
	 */
	for (i = 0; i < 230; i++)
		eq_event(&f, 255, 0);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	for (i = 0; i < 200; i++)
		eq_event(&f, 255, 0);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	for (i = 0; i < 256; i++)
		eq_event(&f, 255, 0);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	TEST_CHECK(f.data.unknown_events == 698);
	/* Completion events and CQ polling still work after the ignore. */
	eq_event(&f, 3, 10);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	cqe(&f.tx, 0x60, 21 | (1U << 24));
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 1);
	TEST_CHECK(completion.wq_id == 21 && completion.is_sq);
	TEST_CHECK(mana_data_post(&f.tx, &work, &posted) == 0);
	TEST_CHECK(mana_data_retire(&f.tx, &posted) == 0);
	/* Stop and shutdown stay recoverable after an undefined event. */
	eq_event(&f, 42, 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.rx) == 0);
	TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
	cqe(&f.rx, 0x43, 22);
	TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 1);
	TEST_CHECK(completion.kind == MANA_DATA_RX_FENCE);
	eq_event(&f, 3, 11);
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
	return stop(&f);
}

/* Service EQE types 129, 130, 131, 133, 134, and 176 are ignored and counted.
 * The pinned adapter EQ sets no service callback, so the pin drops these
 * types there (mana_en.c:1812, gdma_main.c:942-949). The device reconfig
 * type 132 keeps the poison path because the pin reacts with device service
 * work that this library does not have (gdma_main.c:956-971). The reset
 * request type 135 poison stays covered in the bad-events test.
 */
static int service_eq_events(void)
{
	struct fixture f;
	static const uint32_t tolerated[] = { 129, 130, 131, 133, 134, 176 };
	uint32_t i;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	for (i = 0; i < 6; i++) {
		eq_event(&f, tolerated[i], 0);
		TEST_CHECK(mana_data_poll(&f.data) == 0);
		TEST_CHECK(f.data.service_events == i + 1);
		TEST_CHECK(f.data.unknown_events == 0);
		TEST_CHECK(!f.data.quarantined && !f.control.quarantined);
		TEST_CHECK(f.data.ready);
	}
	/* Completion events still notify after the tolerated types. */
	eq_event(&f, 3, 10);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	TEST_CHECK(f.tx.notified);
	/* Stop and shutdown stay recoverable after the tolerated types. */
	eq_event(&f, 42, 0);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	TEST_CHECK(stop(&f) == 0);
	/* The device reconfig type 132 still poisons the shared context. */
	TEST_CHECK(start(&f) == 0);
	eq_event(&f, 132, 0);
	TEST_CHECK(mana_data_poll(&f.data) == -ENOTSUP);
	TEST_CHECK(discard(&f, 7) == 0);
	return 0;
}

/* One poll drains a mixed EQ batch and rings the EQ doorbell once. */
static int eq_batch(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	uint32_t head;
	unsigned int bells;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.rx, MANA_CONTROL_RX,
					  4096, 4096) == 0);
	eq_event(&f, 3, 10); /* TX CQ notification. */
	eq_event(&f, 3, 11); /* RX CQ notification. */
	eq_event(&f, 64, 0); /* Data EQ self-test. */
	eq_event(&f, 255, 0); /* Undefined type. */
	f.data.test_seen = 0;
	head = f.data.eq_head;
	bells = f.eq_bells;
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	TEST_CHECK(f.tx.notified && f.rx.notified);
	TEST_CHECK(f.data.test_seen && f.data.unknown_events == 1);
	TEST_CHECK(f.data.eq_head == head + 4);
	TEST_CHECK(f.eq_bells == bells + 1); /* One doorbell for the batch. */
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	TEST_CHECK(f.eq_bells == bells + 1); /* An empty pass rings nothing. */
	/* Both notified queues still serve completions. */
	cqe(&f.tx, 0x60, 21 | (1U << 24));
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 1);
	TEST_CHECK(f.tx.notified && completion.wq_id == 21);
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 0);
	TEST_CHECK(!f.tx.notified);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	TEST_CHECK(mana_control_fence_rx(&f.rx.object) == 0);
	cqe(&f.rx, 0x43, 22);
	TEST_CHECK(mana_data_poll_cq(&f.rx, &completion) == 1);
	TEST_CHECK(completion.kind == MANA_DATA_RX_FENCE);
	eq_event(&f, 3, 11);
	TEST_CHECK(mana_data_queue_quiesce(&f.rx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.rx) == 0);
	return stop(&f);
}

/* The CQ head crosses the 32-bit counter boundary. Owner generations,
 * doorbell tails, and completion service must continue correctly.
 */
static int cq_wrap32(void)
{
	struct fixture f;
	struct mana_data_completion completion;
	unsigned char *ring;
	unsigned int bells;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_data_queue_create(&f.data, &f.tx, MANA_CONTROL_TX,
					  4096, 4096) == 0);
	/* Simulate 2^32 - 1 units polled. Entry 63 holds owner bits 7. */
	f.tx.cq_head = UINT32_MAX;
	cqe(&f.tx, 0x60, 21 | (1U << 24));
	bells = f.cq_bells;
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 1);
	TEST_CHECK(f.tx.cq_head == 0); /* The 32-bit counter wrapped. */
	TEST_CHECK(completion.wq_id == 21 && completion.is_sq);
	/* The next slot carries the last full pass: consumed bits 7. */
	ring = (unsigned char *)f.tx.cq.dma.addr;
	mana_put32(ring + 60, 7U << 29);
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 0);
	TEST_CHECK(mana_data_poll(&f.data) == 0);
	cqe(&f.tx, 0x60, 21 | (1U << 24)); /* Owner generation restarts at 0. */
	TEST_CHECK(mana_data_poll_cq(&f.tx, &completion) == 1);
	TEST_CHECK(f.tx.cq_head == 1);
	/* Both wrapped arms carried tail 0 then tail 1 with the arm bit. */
	TEST_CHECK(f.cq_bells == bells + 2);
	TEST_CHECK(mana_data_queue_quiesce(&f.tx) == 0);
	TEST_CHECK(mana_data_queue_destroy(&f.tx) == 0);
	return stop(&f);
}

static int eq_timeout(void)
{
	struct fixture f;
	uint64_t before;
	init(&f);
	TEST_CHECK(start_control(&f) == 0);
	f.no_test = 1;
	before = f.model.host.now_us;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -ETIMEDOUT);
	TEST_CHECK(f.model.host.now_us - before == UINT64_C(30000000));
	TEST_CHECK(discard(&f, 7) == 0);
	init(&f);
	TEST_CHECK(start_control(&f) == 0);
	f.no_test = 1;
	f.model.clock_fail = 1;
	TEST_CHECK(mana_data_start(&f.data, &f.control, 4096) == -EIO);
	return discard(&f, 7);
}

int main(void)
{
	static const struct host_test tests[] = {
		{ "Data EQ/WQ/CQ ownership and validated RX fence", lifecycle },
		{ "Three-bit EQ/CQ owner wrap and typed completion", owner_wrap },
		{ "GDMA WQE layout, ring wrap, full queue, and retirement", work_rings },
		{ "RQ long form rejects sixteen SGEs with E2BIG", rq_sge_limit },
		{ "Two-unit long-form WQE needs two free units", long_form_capacity },
		{ "Queue sizes, queried limits, SGE keys, and bounds", input_bounds },
		{ "Every queue allocation failure and invalid DMA", allocation_failures },
		{ "Largest accepted power-of-two region page list", large_ring_boundary },
		{ "Proven firmware failures permit safe cleanup retry", firmware_failures },
		{ "Timeout create quarantines and short create fails proven", uncertain_create },
		{ "Unknown destroy results retain regions and DMA", uncertain_cleanup },
		{ "Invalid identifiers and duplicate handles quarantine", invalid_resources },
		{ "Malformed owners, events, CQ IDs, and RX fence", bad_events },
		{ "Undefined EQ event types stay ignored and recoverable", unknown_eq_events },
		{ "Service EQ events stay ignored and recoverable", service_eq_events },
		{ "One EQ poll drains a mixed batch with one doorbell", eq_batch },
		{ "CQ head wraps the 32-bit counter with fresh owners", cq_wrap32 },
		{ "Bounded EQ test timeout and stalled clock", eq_timeout }
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
