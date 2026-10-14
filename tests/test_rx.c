/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include <uk/mana/rx.h>
#include <stdlib.h>

struct fixture {
	struct host_bootstrap model;
	struct mana_control control;
	struct mana_data data;
	struct mana_rx rx;
	struct mana_rx_slot slots[4];
	uint32_t eq_producer, cq_producer, regions;
	uint64_t handles[3];
	uint32_t failure, timeout;
	unsigned int no_fence, bad_fence, drain_packet, destroys, mismatch;
	unsigned int fail_enable;
	void (*write64)(void *, const struct mana_mapping *, uint64_t, uint64_t);
};

static unsigned char *completion(struct fixture *f, uint32_t type,
				 uint32_t length)
{
	struct mana_data_queue *q = &f->rx.queue;
	uint32_t entries = (uint32_t)q->cq.dma.size / 64;
	unsigned char *entry = (unsigned char *)q->cq.dma.addr +
		(f->cq_producer % entries) * 64;
	memset(entry, 0, 64);
	mana_put32(entry, 0x40 | type);
	mana_put16(entry + 8, (uint16_t)length);
	/* Deliberately nonzero. Linux defines no RX offset validity rule. */
	mana_put32(entry + 56, 0xabcdef98);
	mana_put32(entry + 60, q->object.wq_id |
		   ((f->cq_producer / entries) & 7) << 29);
	f->cq_producer++;
	return entry;
}

static int respond(void *ctx, const unsigned char *req, size_t size,
		   unsigned char *resp, size_t capacity, size_t *length)
{
	struct fixture *f = ctx;
	uint32_t code = mana_get32(req + 4);
	uint32_t i;
	(void)size;
	(void)capacity;
	(void)length;
	if (code == 1 || code == 2 || code == 3 ||
	    (code == 10 && mana_get32(req + 32) == 1))
		return -ENOTSUP;
	if (code == f->timeout)
		f->model.fault = HOST_BOOTSTRAP_NO_TX;
	switch (code) {
	case 4:
		mana_put32(resp + 32, 23);
		mana_put32(resp + 36, 0x91);
		break;
	case 5:
	case 0x20003:
	case 0x2000a:
		break;
	case 0x20007:
		if (f->fail_enable && f->rx.count == 4 && !f->rx.stopping)
			mana_put32(resp + 24, 1);
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
	case 25:
		if (f->regions >= 3)
			abort();
		f->handles[f->regions] = 0x12340001 + f->regions;
		mana_put64(resp + 32, f->handles[f->regions++]);
		break;
	case 27:
		break;
	case 12:
		mana_put32(resp + 32, 17);
		f->eq_producer = 256;
		break;
	case 13:
		break;
	case 10: {
		unsigned char *entry = (unsigned char *)f->data.eq.dma.addr +
			(f->eq_producer % 256) * 16;
		memset(entry, 0, 16);
		mana_put32(entry + 12, 64 |
			   ((f->eq_producer / 256) & 7) << 29);
		f->eq_producer++;
		break;
	}
	case 0x20004:
		if (mana_get32(req + 48) != MANA_CONTROL_RX ||
		    mana_get64(req + 56) != f->handles[1] ||
		    mana_get64(req + 64) != f->handles[2] ||
		    mana_get32(req + 84) != 17)
			f->mismatch = 1;
		for (i = 80; i < 96; i += 4)
			if (i != 84 && mana_get32(req + i))
				f->mismatch = 1;
		mana_put32(resp + 32, 22);
		mana_put32(resp + 36, 11);
		mana_put64(resp + 40, 0x56780002);
		f->cq_producer = mana_get32(req + 76) / 64;
		break;
	case 0x20006:
		if (!f->no_fence && code != f->failure && code != f->timeout) {
			if (f->drain_packet)
				completion(f, 1, 60);
			mana_put32(completion(f, 3, 0),
				   f->bad_fence ? 0x143 : 0x43);
		}
		break;
	case 0x20005:
		f->destroys++;
		break;
	default:
		abort();
	}
	if (code == f->failure)
		mana_put32(resp + 24, 1);
	return 0;
}

static void write64(void *ctx, const struct mana_mapping *m,
		    uint64_t off, uint64_t value)
{
	struct fixture *f = ctx;
	uint32_t id = (uint32_t)value & 0xffffff;
	if (id == 5 || id == 6 || id == 7 || id == 9) {
		f->write64(ctx, m, off, value);
		return;
	}
	if (host_mock_write(&f->model.host, (size_t)off, 8, value))
		abort();
	if (id != 17 && id != 11 && id != 22)
		f->mismatch = 1;
}

static int init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	f->model.respond = respond;
	f->model.respond_ctx = f;
	f->write64 = f->model.platform.write64;
	f->model.platform.write64 = write64;
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_control_start(&f->control, &f->model.b, 0) == 0);
	TEST_CHECK(mana_data_start(&f->data, &f->control, 4096) == 0);
	return 0;
}

static int start(struct fixture *f)
{
	TEST_CHECK(init(f) == 0);
	TEST_CHECK(mana_rx_start(&f->rx, &f->data, f->slots, 4, 4096, 4096) == 0);
	TEST_CHECK(f->rx.ready && f->control.rx_enabled && f->rx.count == 4);
	TEST_CHECK(!f->rx.queue.wq.handle && !f->rx.queue.cq.handle);
	TEST_CHECK(f->rx.queue.wq.consumed && f->rx.queue.cq.consumed);
	return 0;
}

static int end(struct fixture *f, unsigned int packets)
{
	TEST_CHECK(mana_data_stop(&f->data) == 0);
	TEST_CHECK(mana_control_stop(&f->control) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	TEST_CHECK(f->model.host.live_allocations == packets);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	return 0;
}

static int discard(struct fixture *f)
{
	size_t live = f->model.host.live_allocations;
	TEST_CHECK(f->rx.quarantined);
	TEST_CHECK(mana_rx_stop(&f->rx) == -ENOTCONN);
	TEST_CHECK(f->model.host.live_allocations == live);
	/* Release only the simulated device's memory. */
	TEST_CHECK(host_mock_cleanup(&f->model.host) == live);
	return 0;
}

static int lifecycle(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	unsigned char expected[32] = { 0 };
	unsigned int i;
	TEST_CHECK(start(&f) == 0);
	mana_put32(expected + 4, 0x201);
	mana_put64(expected + 16, f.slots[0].dma.device_addr);
	mana_put32(expected + 24, 0x91);
	mana_put32(expected + 28, f.control.mtu + 14);
	TEST_CHECK(!memcmp(expected, f.rx.queue.wq.dma.addr, 32));
	TEST_CHECK(f.slots[0].dma.size == 4096);
	TEST_CHECK(f.slots[0].dma.device_addr != (uintptr_t)f.slots[0].dma.addr);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_EMPTY);
	for (i = 0; i < 1100; i++) {
		struct mana_rx_slot *slot = &f.slots[f.rx.first];
		void *address = slot->dma.addr;
		memset(address, 0x5a, 60);
		unsigned char *entry = completion(&f, 1, 60);
		mana_put32(entry + 4, 1U << 25);
		entry[20] = 0xa5;
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
		TEST_CHECK(packet.dma.addr == address && packet.length == 60);
		TEST_CHECK(packet.offset == 0 && ((unsigned char *)packet.dma.addr)[59] == 0x5a);
		TEST_CHECK(f.rx.count == 3);
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) == -EINVAL);
		TEST_CHECK(mana_rx_refill(&f.rx) == 0 && f.rx.count == 4);
		TEST_CHECK(mana_rx_packet_release(&packet) == 0);
		TEST_CHECK(mana_rx_packet_release(&packet) == -EINVAL);
	}
	TEST_CHECK(f.rx.delivered == 1100);
	completion(&f, 1, 60);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	f.drain_packet = 1;
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	TEST_CHECK(f.destroys == 1 && !f.control.rx_enabled);
	TEST_CHECK(end(&f, 1) == 0);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(host_mock_cleanup(&f.model.host) == 0);
	return 0;
}

static int drops(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	const uint32_t lengths[] = { 0, 13, 1515, 65535, 60, 60 };
	unsigned int i;
	TEST_CHECK(start(&f) == 0);
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		unsigned char *entry = completion(&f, i == 4 ? 4 : 1, lengths[i]);
		if (i == 5)
			mana_put32(entry + 4, 1U << 12);
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_DROPPED);
		TEST_CHECK(!packet.dma.addr && f.rx.count == 3 && !f.rx.quarantined);
		TEST_CHECK(mana_rx_refill(&f.rx) == 0);
	}
	TEST_CHECK(f.rx.dropped == 6);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	TEST_CHECK(end(&f, 0) == 0);
	return 0;
}

static int malformed(void)
{
	unsigned int fault;
	for (fault = 0; fault < 12; fault++) {
		struct fixture f;
		struct mana_rx_packet packet = { 0 };
		unsigned char *entry;
		uint32_t tail;
		TEST_CHECK(start(&f) == 0);
		entry = completion(&f, 1, 60);
		switch (fault) {
		case 0: mana_put32(entry, 0x141); break;
		case 1: mana_put32(entry, 0x42); break;
		case 2: mana_put32(entry, 0x47); break;
		case 3: mana_put32(entry, 0x60); break;
		case 4: mana_put32(entry + 60, 23 | 1U << 29); break;
		case 5: mana_put32(entry + 60, 22 | 1U << 24 | 1U << 29); break;
		case 6: mana_put32(entry + 60, 22 | 3U << 29); break;
		case 7: mana_put32(entry + 4, 1U << 15); break;
		case 8: mana_put16(entry + 10, 1); break;
		case 9: mana_put32(entry + 12, 1); break;
		case 10: f.slots[0].posted.offset = 32; break;
		case 11: f.slots[0].posted.basic_units = 2; break;
		}
		tail = f.rx.queue.tail;
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) < 0);
		TEST_CHECK(!packet.dma.addr && f.rx.count == 4);
		TEST_CHECK(f.rx.queue.tail == tail);
		TEST_CHECK(discard(&f) == 0);
	}
	return 0;
}

static int allocation(void)
{
	unsigned int failure;
	for (failure = 1; failure <= 6; failure++) {
		struct fixture f;
		TEST_CHECK(init(&f) == 0);
		TEST_CHECK(host_fault_arm(&f.model.host, HOST_FAULT_ALLOC, failure, -ENOMEM) == 0);
		TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 4, 4096, 4096) == -ENOMEM);
		TEST_CHECK(!f.rx.ready && !f.rx.quarantined && !f.control.rx_enabled);
		TEST_CHECK(mana_rx_stop(&f.rx) == 0);
		TEST_CHECK(end(&f, 0) == 0);
	}
	return 0;
}

static int refill(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	uint32_t head;
	TEST_CHECK(start(&f) == 0);
	completion(&f, 1, 60);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	TEST_CHECK(host_fault_arm(&f.model.host, HOST_FAULT_ALLOC, 1, -ENOMEM) == 0);
	TEST_CHECK(mana_rx_refill(&f.rx) == -ENOMEM);
	TEST_CHECK(packet.dma.addr && f.rx.count == 3);
	TEST_CHECK(mana_rx_refill(&f.rx) == 0 && f.rx.count == 4);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	completion(&f, 1, 60);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	head = f.rx.queue.head;
	f.rx.queue.head = f.rx.queue.tail + 128;
	TEST_CHECK(mana_rx_refill(&f.rx) == -ENOSPC);
	TEST_CHECK(f.rx.count == 3 && packet.dma.addr);
	f.rx.queue.head = head;
	TEST_CHECK(mana_rx_refill(&f.rx) == 0);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	TEST_CHECK(end(&f, 0) == 0);
	return 0;
}

static int shutdown_errors(void)
{
	unsigned int fault;
	for (fault = 0; fault < 8; fault++) {
		struct fixture f;
		TEST_CHECK(start(&f) == 0);
		switch (fault) {
		case 0: f.no_fence = 1; break;
		case 1: f.bad_fence = 1; break;
		case 2: f.timeout = 0x20006; break;
		case 3: f.timeout = 0x20005; break;
		case 4: f.failure = 0x20006; break;
		case 5: f.failure = 0x20005; break;
		case 6: f.failure = 0x20007; break;
		case 7: f.no_fence = 1; f.model.clock_fail = 1; break;
		}
		TEST_CHECK(mana_rx_stop(&f.rx) < 0);
		TEST_CHECK(f.rx.count == 4 && f.model.host.live_allocations == 13);
		if (fault < 4 || fault == 7) {
			TEST_CHECK(discard(&f) == 0);
		} else {
			TEST_CHECK(!f.rx.quarantined);
			TEST_CHECK(mana_rx_refill(&f.rx) == -ESHUTDOWN);
			f.failure = 0;
			TEST_CHECK(mana_rx_stop(&f.rx) == 0);
			TEST_CHECK(end(&f, 0) == 0);
		}
	}
	return 0;
}

static int configuration(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	TEST_CHECK(init(&f) == 0);
	TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 0, 4096, 4096) == -EINVAL);
	TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 64, 4096, 4096) == -EINVAL);
	TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 4, 64, 4096) == -EINVAL);
	TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 4, 8191, 4096) == -EINVAL);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	TEST_CHECK(mana_control_set_mtu(&f.control, 9000) == 0);
	TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 4, 4096, 4096) == 0);
	TEST_CHECK(f.rx.buffer_size == 9014 && f.slots[0].dma.size == 12288);
	completion(&f, 1, 9014);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	TEST_CHECK(packet.length == 9014 && packet.dma.size == 12288);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(mana_rx_refill(&f.rx) == 0);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	TEST_CHECK(end(&f, 0) == 0);
	return 0;
}

static int start_errors(void)
{
	unsigned int fault;
	for (fault = 0; fault < 4; fault++) {
		struct fixture f;
		TEST_CHECK(init(&f) == 0);
		switch (fault) {
		case 0: f.failure = 0x20004; break;
		case 1: f.timeout = 0x20004; break;
		case 2: f.fail_enable = 1; break;
		case 3: f.model.dma_bad_at = f.model.dma_calls + 3; break;
		}
		TEST_CHECK(mana_rx_start(&f.rx, &f.data, f.slots, 4, 4096, 4096) < 0);
		TEST_CHECK(!f.rx.ready && !f.control.rx_enabled);
		if (fault == 1) {
			TEST_CHECK(discard(&f) == 0);
		} else {
			f.failure = 0;
			f.fail_enable = 0;
			TEST_CHECK(mana_rx_stop(&f.rx) == 0);
			TEST_CHECK(end(&f, 0) == 0);
		}
	}
	return 0;
}

static int quarantine_delivery(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	struct mana_rx_packet other = { 0 };
	size_t live;
	TEST_CHECK(start(&f) == 0);
	completion(&f, 1, 60);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	mana_put32(completion(&f, 1, 60), 0x141);
	TEST_CHECK(mana_rx_poll(&f.rx, &other) == -EPROTO);
	live = f.model.host.live_allocations;
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(f.model.host.live_allocations == live - 1);
	TEST_CHECK(discard(&f) == 0);
	return 0;
}

static int empty_fifo(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	unsigned int i;
	TEST_CHECK(start(&f) == 0);
	for (i = 0; i < 4; i++) {
		completion(&f, 1, 60);
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
		TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	}
	TEST_CHECK(!f.rx.count && f.rx.queue.head == f.rx.queue.tail);
	completion(&f, 1, 60);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == -EPROTO);
	TEST_CHECK(discard(&f) == 0);
	return 0;
}

int main(void)
{
	static const struct host_test tests[] = {
		{ "RX golden WQE, owner wrap, packet ownership, release after stop", lifecycle },
		{ "RX length, truncation, and stripped VLAN drops", drops },
		{ "RX malformed completions retain FIFO buffers", malformed },
		{ "Each RX ring and buffer allocation failure", allocation },
		{ "RX refill allocation and full WQ retry preserve delivered ownership", refill },
		{ "RX fence, clock, steering, and destroy failures", shutdown_errors },
		{ "RX capacity, spare fence entry, and jumbo frame bounds", configuration },
		{ "RX create, steering, and invalid DMA start failures", start_errors },
		{ "Delivered packet release remains safe after RX quarantine", quarantine_delivery },
		{ "RX rejects completion without an outstanding FIFO buffer", empty_fifo },
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
