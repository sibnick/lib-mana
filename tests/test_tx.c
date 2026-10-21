/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include <uk/mana/tx.h>
#include <stdlib.h>

#define RECORDS 128U

struct fixture {
	struct host_bootstrap model;
	struct mana_control control;
	struct mana_data data;
	struct mana_tx tx;
	struct mana_tx_record records[RECORDS];
	struct mana_dma packets[RECORDS];
	uint32_t regions, eq_producer, cq_producer;
	uint32_t status_command, timeout_command;
	uint16_t vport;
	unsigned int short_allowed, packet_mode, packet_fail, packet_bad;
	unsigned int packet_calls, packet_live, packet_frees, wq_bells;
	unsigned int destroys, region_destroys, mismatch;
	unsigned int completed, failed, drain_on_delay;
	uint64_t next_packet;
	int (*alloc)(void *, size_t, struct mana_dma *);
	void (*release)(void *, struct mana_dma *);
	int (*delay)(void *, unsigned int);
	void (*write64)(void *, const struct mana_mapping *, uint64_t, uint64_t);
};

static void event(struct fixture *f, uint32_t type, uint32_t id)
{
	uint32_t entries = (uint32_t)f->data.eq.dma.size / 16;
	unsigned char *entry = (unsigned char *)f->data.eq.dma.addr +
		(f->eq_producer % entries) * 16;
	memset(entry, 0, 16);
	mana_put32(entry, id);
	mana_put32(entry + 12, type | ((f->eq_producer / entries) & 7) << 29);
	f->eq_producer++;
}

static int response(void *ctx, const unsigned char *req, size_t size,
		    unsigned char *resp, size_t capacity, size_t *length)
{
	struct fixture *f = ctx;
	uint32_t code = mana_get32(req + 4);
	(void)size;
	(void)capacity;
	(void)length;
	if (code == 1 || code == 2 || code == 3 ||
	    (code == 10 && mana_get32(req + 32) == 1))
		return -ENOTSUP;
	switch (code) {
	case 4:
		mana_put32(resp + 32, 23);
		mana_put32(resp + 36, 0x91);
		break;
	case 5:
	case 0x20007:
	case 0x2000a:
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
		mana_put16(resp + 32, f->vport);
		resp[34] = (unsigned char)f->short_allowed;
		break;
	case 25:
		f->regions++;
		mana_put64(resp + 32, 0x12340000 + f->regions);
		break;
	case 27:
		f->region_destroys++;
		/* CREATE_WQ_OBJ consumes regions two and three. */
		if (f->tx.queue.object.live || f->tx.queue.object_destroyed)
			f->mismatch = 1;
		break;
	case 12:
		mana_put32(resp + 32, 17);
		f->eq_producer = (uint32_t)f->data.eq.dma.size / 16;
		break;
	case 13:
		break;
	case 10:
		event(f, 64, 0);
		break;
	case 0x20004:
		if (mana_get32(req + 48) != MANA_CONTROL_TX)
			f->mismatch = 1;
		mana_put32(resp + 32, 21);
		mana_put32(resp + 36, 10);
		mana_put64(resp + 40, 0x56780001);
		break;
	case 0x20005:
		f->destroys++;
		if (f->tx.count || f->tx.queue.head != f->tx.queue.tail ||
		    !f->tx.queue.stopped || f->packet_live)
			f->mismatch = 1;
		break;
	default:
		abort();
	}
	if (code == f->status_command)
		mana_put32(resp + 24, 1);
	if (code == f->timeout_command)
		f->model.fault = HOST_BOOTSTRAP_NO_TX;
	return 0;
}

static void write64(void *ctx, const struct mana_mapping *mapping,
		    uint64_t offset, uint64_t value)
{
	struct fixture *f = ctx;
	uint32_t id = (uint32_t)value & 0xffffff;
	if ((offset == 0x1000 && id == 7) ||
	    (offset == 0x1400 && id == 6) ||
	    (offset == 0x1800 && id == 5) ||
	    (offset == 0x1ff8 && id == 9)) {
		f->write64(ctx, mapping, offset, value);
		return;
	}
	if (host_mock_write(&f->model.host, (size_t)offset, 8, value))
		abort();
	if (offset == 0x1000 && id == 21) {
		f->wq_bells++;
		if ((uint32_t)(value >> 32) != f->tx.queue.head * 32)
			f->mismatch = 1;
	} else if (!((offset == 0x1800 && id == 10) ||
		     (offset == 0x1ff8 && id == 17))) {
		f->mismatch = 1;
	}
}

static int packet_alloc(void *ctx, size_t size, struct mana_dma *dma)
{
	struct fixture *f = ctx;
	unsigned int i;
	if (!f->packet_mode)
		return f->alloc(ctx, size, dma);
	f->packet_calls++;
	if (f->packet_fail)
		return -ENOMEM;
	for (i = 0; i < RECORDS; i++)
		if (!f->packets[i].addr)
			break;
	if (i == RECORDS)
		return -ENOSPC;
	dma->addr = aligned_alloc(4096, size);
	if (!dma->addr)
		return -ENOMEM;
	dma->size = size;
	dma->device_addr = f->next_packet;
	f->next_packet += size;
	memset(dma->addr, 0xcc, size);
	f->packets[i] = *dma;
	f->packet_live++;
	switch (f->packet_bad) {
	case 1: dma->device_addr++; break;
	case 2: dma->size--; break;
	case 3: dma->device_addr = UINT64_MAX & ~UINT64_C(4095); break;
	}
	return 0;
}

static void packet_free(void *ctx, struct mana_dma *dma)
{
	struct fixture *f = ctx;
	unsigned int i;
	for (i = 0; i < RECORDS; i++)
		if (f->packets[i].addr == dma->addr && dma->addr) {
			free(dma->addr);
			memset(&f->packets[i], 0, sizeof(f->packets[i]));
			memset(dma, 0, sizeof(*dma));
			f->packet_live--;
			f->packet_frees++;
			return;
		}
	f->release(ctx, dma);
}

static unsigned char *completion(struct fixture *f, uint32_t type,
				 uint32_t vendor)
{
	struct mana_data_queue *q = &f->tx.queue;
	uint32_t entries = (uint32_t)q->cq.dma.size / 64;
	unsigned char *entry = (unsigned char *)q->cq.dma.addr +
		(f->cq_producer % entries) * 64;
	memset(entry, 0, 64);
	mana_put32(entry, 0x40 | type | vendor << 8);
	/* Linux does not interpret these diagnostic offsets or reserved bytes. */
	mana_put32(entry + 4, 0x76543210);
	mana_put32(entry + 8, 0xabcdef07);
	mana_put32(entry + 56, 0x11223344);
	mana_put32(entry + 60, 21 | 1U << 24 |
		   ((f->cq_producer / entries) & 7) << 29);
	f->cq_producer++;
	return entry;
}

static int delay(void *ctx, unsigned int us)
{
	struct fixture *f = ctx;
	if (f->drain_on_delay && f->tx.count) {
		completion(f, 32, 0);
		f->drain_on_delay = 0;
	}
	return f->delay(ctx, us);
}

static void completed(void *ctx, const struct mana_tx_result *result)
{
	struct fixture *f = ctx;
	if (result->cookie != &f->records[f->completed] ||
	    result->length != 60)
		f->mismatch = 1;
	f->completed++;
	f->failed += result->error != 0;
}

static int start_data(struct fixture *f, uint16_t vport, unsigned int short_form)
{
	memset(f, 0, sizeof(*f));
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	f->vport = vport;
	f->short_allowed = short_form;
	f->next_packet = UINT64_C(0x800000000);
	f->model.respond = response;
	f->model.respond_ctx = f;
	f->write64 = f->model.platform.write64;
	f->alloc = f->model.platform.dma_alloc;
	f->release = f->model.platform.dma_free;
	f->delay = f->model.platform.delay_us;
	f->model.platform.write64 = write64;
	f->model.platform.dma_alloc = packet_alloc;
	f->model.platform.dma_free = packet_free;
	f->model.platform.delay_us = delay;
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_control_start(&f->control, &f->model.b, 0) == 0);
	TEST_CHECK(mana_data_start(&f->data, &f->control, 4096) == 0);
	return 0;
}

static int start(struct fixture *f, uint16_t vport, unsigned int short_form,
		 uint32_t capacity)
{
	TEST_CHECK(start_data(f, vport, short_form) == 0);
	TEST_CHECK(mana_tx_start(&f->tx, &f->data, f->records, capacity,
				4096, 8192) == 0);
	f->cq_producer = f->tx.queue.cq_head;
	f->packet_mode = 1;
	return 0;
}

static int stop_data(struct fixture *f)
{
	f->packet_mode = 0;
	TEST_CHECK(!f->packet_live);
	TEST_CHECK(mana_data_stop(&f->data) == 0);
	TEST_CHECK(mana_control_stop(&f->control) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	return 0;
}

static int stop(struct fixture *f)
{
	TEST_CHECK(mana_tx_stop(&f->tx, completed, f) == 0);
	TEST_CHECK(!f->tx.data && !f->packet_live);
	TEST_CHECK(f->region_destroys == 0 && f->destroys == 1);
	return stop_data(f);
}

static int discard(struct fixture *f, unsigned int pending)
{
	unsigned int i;
	TEST_CHECK(f->tx.quarantined && f->data.quarantined);
	TEST_CHECK(f->control.quarantined && f->tx.count == pending);
	TEST_CHECK(f->packet_live == pending && !f->completed && !f->destroys);
	TEST_CHECK(mana_tx_stop(&f->tx, completed, f) == -ENOTCONN);
	TEST_CHECK(mana_data_stop(&f->data) == -ENOTCONN);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	TEST_CHECK(f->model.host.live_allocations == 9);
	TEST_CHECK(!f->mismatch && !f->model.mismatch);
	/* Dispose only the simulated device after checking retained ownership. */
	for (i = 0; i < RECORDS; i++)
		free(f->packets[i].addr);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 9);
	return 0;
}

static int golden(void)
{
	const uint16_t vports[] = { 0x7f, 0xff, 0x100, 0xfff, 0x7f };
	const unsigned int shorts[] = { 1, 1, 1, 1, 0 };
	unsigned char frame[60], expected[64];
	struct fixture f;
	struct mana_tx_result result;
	unsigned int i;
	memset(frame, 0xa5, sizeof(frame));
	for (i = 0; i < sizeof(vports) / sizeof(vports[0]); i++) {
		uint32_t oob = shorts[i] && vports[i] <= 0xff ? 8 : 24;
		TEST_CHECK(start(&f, vports[i], shorts[i], 8) == 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), frame) == 1);
		TEST_CHECK(f.tx.count == 1 && f.packet_live == 1 && f.wq_bells == 1);
		TEST_CHECK(!f.tx.queue.wq.handle && !f.tx.queue.cq.handle);
		TEST_CHECK(f.tx.queue.wq.consumed && f.tx.queue.cq.consumed);
		memset(expected, 0, sizeof(expected));
		mana_put32(expected + 4, 1 | (oob / 4) << 8);
		mana_put32(expected + 8, 10U << 8 | (oob == 24));
		if (oob == 8)
			mana_put32(expected + 12, (uint32_t)vports[i] << 24);
		else
			mana_put32(expected + 20, (uint32_t)vports[i] << 16);
		mana_put64(expected + 8 + oob, f.records[0].dma.device_addr);
		mana_put32(expected + 16 + oob, 0x91);
		mana_put32(expected + 20 + oob, 60);
		TEST_CHECK(!memcmp(expected, f.tx.queue.wq.dma.addr,
				   oob == 8 ? 32 : 64));
		TEST_CHECK(!memcmp(frame, f.records[0].dma.addr, sizeof(frame)));
		memset(frame, 0x5a, sizeof(frame));
		TEST_CHECK(((unsigned char *)f.records[0].dma.addr)[0] == 0xa5);
		completion(&f, 32, 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.cookie == frame && !result.error && result.type == 32);
		TEST_CHECK(result.data_offset == 0x76543210 && result.sgl_offset == 7);
		TEST_CHECK(result.wqe_offset == (0xabcdef07U >> 5));
		TEST_CHECK(!f.tx.count && !f.packet_live && f.packet_frees == 1);
		TEST_CHECK(f.tx.queue.head == f.tx.queue.tail);
		TEST_CHECK(stop(&f) == 0);
		memset(frame, 0xa5, sizeof(frame));
	}
	return 0;
}

static int lengths(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[9015];
	uint32_t length;
	memset(frame, 0xa5, sizeof(frame));
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, NULL, 60, NULL) == -EMSGSIZE);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 13, NULL) == -EMSGSIZE);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 1515, NULL) == -EMSGSIZE);
	TEST_CHECK(!f.packet_calls && !f.wq_bells);
	for (length = 14; length <= 60; length += 46) {
		unsigned int j;
		TEST_CHECK(mana_tx_submit(&f.tx, frame, length, NULL) == 1);
		for (j = length; j < 4096; j++)
			TEST_CHECK(((unsigned char *)f.records[f.tx.first].dma.addr)[j] == 0);
		completion(&f, 32, 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.length == length && result.wire_length == 60);
	}
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 1514, NULL) == 1);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.wire_length == 1514);
	TEST_CHECK(stop(&f) == 0);
	TEST_CHECK(start_data(&f, 0, 1) == 0);
	TEST_CHECK(mana_control_set_mtu(&f.control, 9000) == 0);
	TEST_CHECK(mana_tx_start(&f.tx, &f.data, f.records, 8, 4096, 8192) == 0);
	f.packet_mode = 1;
	f.cq_producer = f.tx.queue.cq_head;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 9014, NULL) == 1);
	TEST_CHECK(f.records[0].dma.size == 12288);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 9015, NULL) == -EMSGSIZE);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.wire_length == 9014);
	return stop(&f);
}

static int full_and_wrap(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60] = { 0 };
	unsigned int i;
	TEST_CHECK(start(&f, 0x100, 1, RECORDS) == 0);
	for (i = 0; i < 64; i++)
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[i]) == 1);
	TEST_CHECK(f.tx.queue.head - f.tx.queue.tail == 128 && f.tx.count == 64);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == -ENOSPC);
	TEST_CHECK(f.wq_bells == 64 && f.packet_calls == 64);
	for (i = 0; i < 64; i++) {
		completion(&f, 32, 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.cookie == &f.records[i]);
	}
	for (i = 0; i < 1024; i++) {
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
		TEST_CHECK(f.records[f.tx.first].posted.offset == (i % 64) * 64);
		completion(&f, 32, 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1 && result.cookie == frame);
	}
	f.tx.queue.head = f.tx.queue.tail = UINT32_MAX - 1;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
	TEST_CHECK(f.tx.queue.head == 0 && f.records[f.tx.first].posted.offset == 4032);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(f.tx.queue.tail == 0);
	TEST_CHECK(stop(&f) == 0);
	TEST_CHECK(start(&f, 0, 1, 2) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == 1);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == 1);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == -ENOSPC);
	TEST_CHECK(f.packet_calls == 2 && f.wq_bells == 2);
	completion(&f, 32, 0);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	return stop(&f);
}

static int packet_errors(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60] = { 0 };
	uint32_t type, vendor;
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	for (vendor = 0; vendor <= 1; vendor++)
		for (type = 32; type <= 41; type++) {
			TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
			completion(&f, type, vendor ? 0x123456 : 0);
			TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
			TEST_CHECK(result.error == (type == 32 ? 0 : -EIO));
			TEST_CHECK(result.type == type && result.cookie == frame);
			TEST_CHECK(result.vendor_error == (vendor ? 0x123456U : 0));
			TEST_CHECK(!f.tx.quarantined && !f.packet_live);
		}
	return stop(&f);
}

/*
 * An unknown TX completion type completes the packet with an error.
 * The pinned poll logs the type and still frees the packet
 * (mana_poll_tx_cq, mana_en.c:2002-2011). Identity mismatches stay fatal.
 */
static int unknown_type(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60] = { 0 };
	static const uint32_t unknown[] = { 0, 1, 31, 42, 63 };
	uint32_t i;
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	for (i = 0; i < 5; i++) {
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
		completion(&f, unknown[i], 0);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.error == -EIO);
		TEST_CHECK(result.type == unknown[i]);
		TEST_CHECK(result.vendor_error == 0);
		TEST_CHECK(result.cookie == frame);
		TEST_CHECK(result.length == 60 && result.wire_length == 60);
		TEST_CHECK(!f.tx.quarantined && !f.packet_live);
	}
	return stop(&f);
}

static int malformed(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60] = { 0 }, *entry;
	unsigned int fault;
	for (fault = 0; fault < 8; fault++) {
		TEST_CHECK(start(&f, 0, 1, 8) == 0);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
		entry = completion(&f, 32, 0);
		switch (fault) {
		case 0: mana_put32(entry, 0x80 | 32); break;
		case 1: mana_put32(entry + 60, 1U << 29 | 1U << 24 | 22); break;
		case 2: mana_put32(entry + 60, 1U << 29 | 21); break;
		case 3: mana_put32(entry + 60, 3U << 29 | 1U << 24 | 21); break;
		case 4: mana_put32(entry + 60, 1U << 29 | 1U << 25 | 1U << 24 | 21); break;
		case 5: f.records[0].posted.offset = 32; break;
		case 6: f.records[0].posted.basic_units = 2; break;
		default: f.records[0].wire_length = 4097; break;
		}
		TEST_CHECK(mana_tx_poll(&f.tx, &result) < 0);
		TEST_CHECK(f.tx.queue.tail == 0 && !f.packet_frees);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == -ENOTCONN);
		TEST_CHECK(discard(&f, 1) == 0);
	}
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == -EPROTO);
	return discard(&f, 0);
}

static int allocations(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[9014] = { 0 };
	unsigned int bad;
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	f.packet_fail = 1;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == -ENOMEM);
	TEST_CHECK(!f.tx.count && !f.packet_live && !f.wq_bells);
	f.packet_fail = 0;
	for (bad = 1; bad <= 2; bad++) {
		f.packet_bad = bad;
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == -EPROTO);
		TEST_CHECK(!f.tx.count && !f.packet_live && !f.wq_bells);
	}
	f.packet_bad = 0;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
	f.packet_fail = 1;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == -ENOMEM);
	TEST_CHECK(f.tx.count == 1 && f.packet_live == 1 && f.wq_bells == 1);
	f.packet_fail = 0;
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(stop(&f) == 0);
	TEST_CHECK(start_data(&f, 0, 1) == 0);
	TEST_CHECK(mana_control_set_mtu(&f.control, 9000) == 0);
	TEST_CHECK(mana_tx_start(&f.tx, &f.data, f.records, 8, 4096, 8192) == 0);
	f.packet_mode = 1;
	f.packet_bad = 3;
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), frame) == -EPROTO);
	TEST_CHECK(!f.tx.count && !f.packet_live && !f.wq_bells);
	return stop(&f);
}

static int shutdown(void)
{
	struct fixture f;
	unsigned char frame[60] = { 0 };
	unsigned int fault;
	uint64_t before;
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[0]) == 1);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[1]) == 1);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == -EINVAL);
	TEST_CHECK(f.tx.ready && !f.tx.stopped && f.tx.count == 2);
	completion(&f, 33, 0);
	completion(&f, 32, 0);
	TEST_CHECK(stop(&f) == 0 && f.completed == 2 && f.failed == 1);
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[0]) == 1);
	f.drain_on_delay = 1;
	TEST_CHECK(stop(&f) == 0 && f.completed == 1);
	for (fault = 0; fault < 2; fault++) {
		TEST_CHECK(start(&f, 0, 1, 8) == 0);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[0]) == 1);
		before = f.model.host.now_us;
		if (fault)
			f.model.clock_fail = 1;
		TEST_CHECK(mana_tx_stop(&f.tx, completed, &f) ==
			   (fault ? -EIO : -ETIMEDOUT));
		TEST_CHECK(f.model.host.now_us - before ==
			   (fault ? 0 : MANA_TX_DRAIN_POLLS * UINT64_C(1000)));
		TEST_CHECK(discard(&f, 1) == 0);
	}
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	f.status_command = 0x20005;
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == -EPROTO);
	TEST_CHECK(!f.tx.quarantined && f.tx.stopped && f.tx.queue.object.live);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, NULL) == -ESHUTDOWN);
	f.status_command = 0;
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	TEST_CHECK(f.destroys == 2 && f.region_destroys == 0);
	TEST_CHECK(stop_data(&f) == 0);
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	f.timeout_command = 0x20005;
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == -ETIMEDOUT);
	TEST_CHECK(f.tx.quarantined && f.model.host.live_allocations == 9);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == -ENOTCONN);
	TEST_CHECK(host_mock_cleanup(&f.model.host) == 9);
	return 0;
}

static int failed_start(void)
{
	struct fixture f;
	unsigned int fail;
	for (fail = 1; fail <= 2; fail++) {
		TEST_CHECK(start_data(&f, 0, 1) == 0);
		TEST_CHECK(host_fault_arm(&f.model.host, HOST_FAULT_ALLOC,
					 fail, -ENOMEM) == 0);
		TEST_CHECK(mana_tx_start(&f.tx, &f.data, f.records, 8,
					4096, 8192) == -ENOMEM);
		TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
		TEST_CHECK(stop_data(&f) == 0);
	}
	TEST_CHECK(start_data(&f, 0, 1) == 0);
	f.status_command = 0x20004;
	TEST_CHECK(mana_tx_start(&f.tx, &f.data, f.records, 8,
				4096, 8192) == -EPROTO);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	TEST_CHECK(f.region_destroys == 2);
	f.status_command = 0;
	TEST_CHECK(stop_data(&f) == 0);
	TEST_CHECK(start_data(&f, 0x1000, 1) == 0);
	TEST_CHECK(mana_tx_start(&f.tx, &f.data, f.records, 8,
				4096, 8192) == -EINVAL);
	TEST_CHECK(!f.tx.data && !f.data.queues[0]);
	return stop_data(&f);
}

static int event_quarantine(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60] = { 0 };
	unsigned int fault;
	for (fault = 0; fault < 2; fault++) {
		TEST_CHECK(start(&f, 0, 1, 8) == 0);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
		if (fault)
			f.model.b.error = -EPROTO;
		else
			event(&f, 3, 999);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == -EPROTO);
		TEST_CHECK(!f.packet_frees && f.tx.queue.tail == 0);
		TEST_CHECK(discard(&f, 1) == 0);
	}
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, frame) == 1);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	completion(&f, 32, 0);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == -EPROTO);
	TEST_CHECK(f.packet_frees == 1 && !f.tx.count);
	return discard(&f, 0);
}

static int partial_drain(void)
{
	struct fixture f;
	unsigned char frame[60] = { 0 };
	TEST_CHECK(start(&f, 0, 1, 8) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[0]) == 1);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, 60, &f.records[1]) == 1);
	completion(&f, 34, 0);
	TEST_CHECK(mana_tx_stop(&f.tx, completed, &f) == -ETIMEDOUT);
	TEST_CHECK(f.completed == 1 && f.failed == 1);
	TEST_CHECK(f.tx.count == 1 && f.packet_live == 1 && f.packet_frees == 1);
	TEST_CHECK(f.tx.queue.tail == 1 && f.records[1].cookie == &f.records[1]);
	TEST_CHECK(f.tx.quarantined && f.data.quarantined && !f.destroys);
	TEST_CHECK(mana_tx_stop(&f.tx, completed, &f) == -ENOTCONN);
	TEST_CHECK(f.completed == 1 && f.model.host.live_allocations == 9);
	TEST_CHECK(!f.mismatch && !f.model.mismatch);
	free(f.packets[1].addr);
	TEST_CHECK(host_mock_cleanup(&f.model.host) == 9);
	return 0;
}

int main(void)
{
	static const struct host_test tests[] = {
		{ "TX golden short and long wire fields", golden },
		{ "TX frame bounds padding and multi-page DMA", lengths },
		{ "TX WQ and FIFO full plus owner and counter wrap", full_and_wrap },
		{ "TX defined packet errors and vendor status", packet_errors },
		{ "Unknown TX completion type completes the packet", unknown_type },
		{ "TX malformed completions and posted records", malformed },
		{ "TX packet allocation failure and contract", allocations },
		{ "TX shutdown drain timeout clock and destroy", shutdown },
		{ "TX failed start cleanup and consumed regions", failed_start },
		{ "TX shared event errors and duplicate completion", event_quarantine },
		{ "TX partial drain returns only proven results", partial_drain },
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
