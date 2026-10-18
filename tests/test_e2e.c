/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * End-to-end host flows over the asynchronous device model. Each test
 * runs the real driver stack: bootstrap, control, data EQ, TX, and RX.
 * The device model owns all timing. The tests only submit work, tick
 * the device clock, and poll.
 */
#include "host/test.h"
#include "host/device.h"
#include "../src/internal.h"
#include <stdlib.h>
#include <string.h>
#include <uk/mana/rx.h>
#include <uk/mana/tx.h>

struct region {
	uint64_t handle, address;
	uint32_t size;
	unsigned int live, consumed;
};

struct fixture {
	struct host_bootstrap model;
	struct host_device dev;
	struct mana_control control;
	struct mana_data data;
	struct mana_tx tx;
	struct mana_rx rx;
	struct mana_tx_record records[8];
	struct mana_rx_slot slots[4];
	struct region regions[8];
	uint32_t region_count;
	unsigned int mismatch, commands, eq_disables;
	unsigned int object_destroys, region_destroys;
};

static struct region *find_region(struct fixture *f, uint64_t handle)
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
	uint32_t code = mana_get32(req + 4), device = mana_get32(req + 32);
	struct region *r;
	unsigned int i, type;

	(void)size;
	(void)capacity;
	(void)length;
	if (code == 1 || code == 2 || code == 3 || (code == 10 && device == 1))
		return -ENOTSUP; /* The bootstrap model answers HWC setup. */
	f->commands++;
	switch (code) {
	case 4: /* REGISTER_DEVICE */
		mana_put32(resp + 32, 23);
		mana_put32(resp + 36, 0x91);
		mana_put32(resp + 40, 0);
		break;
	case 5: /* DEREGISTER_DEVICE */
		break;
	case 0x20001: /* QUERY_DEVICE */
		mana_put16(resp + 64, 1);
		mana_put32(resp + 68, 4);
		mana_put16(resp + 72, 1514);
		break;
	case 0x20008: /* QUERY_PORT */
		mana_put32(resp + 32, 2);
		mana_put32(resp + 36, 2);
		mana_put32(resp + 40, 64);
		memcpy(resp + 48, "\x02\x11\x22\x33\x44\x55", 6);
		mana_put64(resp + 56, 0x12345678);
		break;
	case 0x20003: /* CONFIG_TX */
		mana_put16(resp + 32, 5);
		resp[34] = 1; /* Short form, 8-byte OOB. */
		break;
	case 0x20007: /* CONFIG_RX */
		break;
	case 0x2000a: /* QUERY_LINK */
		mana_put32(resp + 32, 10000);
		mana_put32(resp + 40, 10000);
		break;
	case 25: /* CREATE_DMA_REGION */
		if (f->region_count >= 8) {
			f->mismatch++;
			break;
		}
		r = &f->regions[f->region_count++];
		r->handle = 0x12340000 + f->region_count;
		r->address = mana_get64(req + 64);
		r->size = mana_get32(req + 40);
		r->live = 1;
		r->consumed = 0;
		mana_put64(resp + 32, r->handle);
		break;
	case 27: /* DESTROY_DMA_REGION */
		r = find_region(f, mana_get64(req + 40));
		if (!r || !r->consumed)
			f->mismatch++;
		else
			r->live = 0;
		f->region_destroys++;
		break;
	case 12: /* CREATE_QUEUE for the data EQ */
		r = find_region(f, mana_get64(req + 56));
		if (!r || r->consumed || mana_get32(req + 68) != 4096)
			f->mismatch++;
		else
			r->consumed = 1;
		mana_put32(resp + 32, 17);
		break;
	case 13: /* DISABLE_QUEUE for the data EQ */
		if (mana_get32(req + 44) != 17)
			f->mismatch++;
		else
			f->eq_disables++;
		break;
	case 10: /* The data EQ self-test. The device model adds the EQE. */
		if (device != 0x10002 || mana_get32(req + 40) != 17)
			f->mismatch++;
		break;
	case 0x20004: /* CREATE_WQ */
		type = mana_get32(req + 48);
		for (i = 0; i < 2; i++) {
			r = find_region(f, mana_get64(req + 56 + i * 8));
			if (!r || r->consumed ||
			    mana_get32(req + 72 + i * 4) != r->size)
				f->mismatch++;
			else
				r->consumed = 1;
		}
		if (mana_get32(req + 84) != 17)
			f->mismatch++;
		mana_put32(resp + 32, type == 1 ? 21 : 22);
		mana_put32(resp + 36, type == 1 ? 10 : 11);
		mana_put64(resp + 40, 0x56780000U + type);
		break;
	case 0x20005: /* DESTROY_WQ */
		f->object_destroys++;
		break;
	case 0x20006: /* FENCE_RX. The device model adds the fence CQE. */
		break;
	default:
		abort();
	}
	return 0;
}

static void init_fixture(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	host_device_attach(&f->model, &f->dev);
	f->dev.inner_respond = respond;
	f->dev.inner_respond_ctx = f;
}

static int start_stack(struct fixture *f)
{
	init_fixture(f);
	host_device_note_data(&f->dev, &f->data);
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_control_start(&f->control, &f->model.b, 0) == 0);
	TEST_CHECK(mana_data_start(&f->data, &f->control, 4096) == 0);
	return 0;
}

static int start_pair(struct fixture *f)
{
	TEST_CHECK(start_stack(f) == 0);
	TEST_CHECK(mana_tx_start(&f->tx, &f->data, f->records, 8, 4096,
				 4096) == 0);
	host_device_note_queue(&f->dev, &f->tx.queue);
	TEST_CHECK(mana_rx_start(&f->rx, &f->data, f->slots, 4, 4096,
				 4096) == 0);
	host_device_note_queue(&f->dev, &f->rx.queue);
	return 0;
}

static int stop_pair(struct fixture *f)
{
	TEST_CHECK(mana_rx_stop(&f->rx) == 0);
	host_device_forget_queue(&f->dev, &f->rx.queue);
	TEST_CHECK(mana_tx_stop(&f->tx, NULL, NULL) == 0);
	host_device_forget_queue(&f->dev, &f->tx.queue);
	return 0;
}

static int stop_down(struct fixture *f)
{
	TEST_CHECK(mana_data_stop(&f->data) == 0);
	host_device_note_data(&f->dev, NULL);
	TEST_CHECK(mana_control_stop(&f->control) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	host_device_detach(&f->dev);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	TEST_CHECK(f->dev.work_count == 0 && f->dev.work_drops == 0);
	TEST_CHECK(f->dev.eq_guards == 0 && f->dev.cq_guards == 0);
	TEST_CHECK(f->dev.bad_doorbells == 0 && f->dev.ctrl_pends == 0);
	TEST_CHECK(f->eq_disables == 1);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	return 0;
}

static void make_frame(unsigned char *frame, size_t length, unsigned int seed)
{
	size_t i;

	for (i = 0; i < length; i++)
		frame[i] = (unsigned char)(i + seed * 3U + 1U);
	frame[12] = 0x08;
	frame[13] = 0x00;
}

static unsigned int drain_calls;
static void *drain_cookie[8];
static int drain_error[8];

static void drain_cb(void *ctx, const struct mana_tx_result *result)
{
	if (drain_calls < 8) {
		drain_cookie[drain_calls] = result->cookie;
		drain_error[drain_calls] = result->error;
	}
	(*(unsigned int *)ctx)++;
}

static int full_round_trip(void)
{
	struct fixture f;
	struct mana_tx_result result;
	struct mana_rx_packet packet = { 0 };
	unsigned char out[64], in[80];
	unsigned int i;

	make_frame(out, sizeof(out), 1);
	make_frame(in, sizeof(in), 2);
	TEST_CHECK(start_pair(&f) == 0);

	/* The device reads the exact wire image the driver posted. */
	TEST_CHECK(mana_tx_submit(&f.tx, out, sizeof(out), (void *)7) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.cookie == (void *)7 && result.error == 0);
	TEST_CHECK(f.dev.last_tx_length == 64);
	for (i = 0; i < sizeof(out); i++)
		TEST_CHECK(f.dev.last_tx_frame[i] == out[i]);

	/* One injected frame arrives exactly once with intact bytes. */
	TEST_CHECK(host_device_rx_inject(&f.dev, in, sizeof(in)) == 0);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	TEST_CHECK(packet.length == 80);
	for (i = 0; i < sizeof(in); i++)
		TEST_CHECK(((unsigned char *)packet.dma.addr)[i] == in[i]);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_EMPTY);
	TEST_CHECK(f.rx.delivered == 1 && f.rx.dropped == 0);

	/* Every queue tears down through the same asynchronous device. */
	TEST_CHECK(stop_pair(&f) == 0);
	TEST_CHECK(f.object_destroys == 2);
	TEST_CHECK(f.dev.tx_frames == 1);
	return stop_down(&f);
}

static int stop_drains_async(void)
{
	struct fixture f;
	unsigned char frame[60];
	uint64_t before;

	make_frame(frame, sizeof(frame), 3);
	TEST_CHECK(start_pair(&f) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)2) == 1);
	/* No test tick. Only the driver's own delay loop may finish work. */
	before = f.dev.now_us;
	drain_calls = 0;
	TEST_CHECK(mana_tx_stop(&f.tx, drain_cb, &drain_calls) == 0);
	TEST_CHECK(drain_calls == 2);
	TEST_CHECK(drain_cookie[0] == (void *)1 && drain_cookie[1] == (void *)2);
	TEST_CHECK(drain_error[0] == 0 && drain_error[1] == 0);
	TEST_CHECK(f.dev.now_us > before);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	host_device_forget_queue(&f.dev, &f.rx.queue);
	return stop_down(&f);
}

static int stall_quarantine(void)
{
	struct fixture f;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame), 4);
	TEST_CHECK(start_pair(&f) == 0);
	host_device_stall_data(&f.dev, true);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	drain_calls = 0;
	TEST_CHECK(mana_tx_stop(&f.tx, drain_cb, &drain_calls) == -ETIMEDOUT);
	TEST_CHECK(drain_calls == 0);
	TEST_CHECK(f.tx.quarantined && f.data.quarantined);
	/* The quarantine contract: nothing frees, nothing is reused. */
	TEST_CHECK(mana_rx_stop(&f.rx) == -ENOTCONN);
	TEST_CHECK(mana_data_stop(&f.data) == -ENOTCONN);
	TEST_CHECK(mana_control_stop(&f.control) == -ENOTCONN);
	TEST_CHECK(mana_bootstrap_stop(&f.model.b) == -EBUSY);
	/* 6 HWC + EQ + 2 TX + 2 RX + 4 RX buffers + 1 TX record. */
	TEST_CHECK(f.model.host.live_allocations == 16);
	TEST_CHECK(host_mock_cleanup(&f.model.host) == 16);
	host_device_detach(&f.dev);
	return 0;
}

static int interleaved_traffic(void)
{
	struct fixture f;
	struct mana_tx_result result;
	struct mana_rx_packet packet = { 0 };
	unsigned char frame[70];
	unsigned int i, delivered = 0;

	TEST_CHECK(start_pair(&f) == 0);
	for (i = 0; i < 8; i++) {
		make_frame(frame, sizeof(frame), i + 1);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame),
					  (void *)(uintptr_t)(i + 1)) == 1);
		TEST_CHECK(host_device_rx_inject(&f.dev, frame,
						 sizeof(frame)) == 0);
		host_device_tick(&f.dev, 8);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.cookie == (void *)(uintptr_t)(i + 1));
		TEST_CHECK(result.error == 0);
		for (unsigned int j = 0; j < sizeof(frame); j++)
			TEST_CHECK(f.dev.last_tx_frame[j] == frame[j]);
		TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
		for (unsigned int j = 0; j < sizeof(frame); j++)
			TEST_CHECK(((unsigned char *)packet.dma.addr)[j] ==
				   frame[j]);
		TEST_CHECK(mana_rx_packet_release(&packet) == 0);
		delivered++;
		TEST_CHECK(mana_rx_refill(&f.rx) == 0);
	}
	TEST_CHECK(delivered == 8 && f.rx.delivered == 8);
	TEST_CHECK(f.dev.tx_frames == 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_EMPTY);
	TEST_CHECK(stop_pair(&f) == 0);
	return stop_down(&f);
}

static int unknown_eqe_recovery(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];
	static const uint32_t types[] = { 5, 100, 177, 250 };
	uint32_t i;

	make_frame(frame, sizeof(frame), 5);
	TEST_CHECK(start_pair(&f) == 0);
	for (i = 0; i < 4; i++) {
		TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame),
					  (void *)(uintptr_t)(i + 1)) == 1);
		/* An undefined event lands while the packet is in flight. */
		TEST_CHECK(host_device_eqe(&f.dev, types[i], 0) == 0);
		host_device_tick(&f.dev, 16); /* Past the event latency. */
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.error == 0);
		TEST_CHECK(f.data.unknown_events == i + 1);
		TEST_CHECK(f.data.ready && !f.data.quarantined);
	}
	TEST_CHECK(stop_pair(&f) == 0);
	return stop_down(&f);
}

static int link_flap(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];
	uint32_t generation;

	make_frame(frame, sizeof(frame), 6);
	TEST_CHECK(start_pair(&f) == 0);
	generation = f.model.b.link_generation;
	TEST_CHECK(host_device_bootstrap_event(&f.dev, 133, 3U << 24) == 0);
	host_device_tick(&f.dev, 10);
	TEST_CHECK(mana_control_poll(&f.control) == 0);
	TEST_CHECK(f.model.b.link_known && !f.model.b.link_up);
	/* The datapath keeps running while the link is down. */
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0);
	TEST_CHECK(host_device_bootstrap_event(&f.dev, 133, 2U << 24) == 0);
	host_device_tick(&f.dev, 10);
	TEST_CHECK(mana_control_poll(&f.control) == 0);
	TEST_CHECK(f.model.b.link_up);
	TEST_CHECK(f.model.b.link_generation == generation + 2);
	TEST_CHECK(stop_pair(&f) == 0);
	return stop_down(&f);
}

int main(void)
{
	static const struct host_test tests[] = {
		{ "Full lifecycle crosses the asynchronous device round trip", full_round_trip },
		{ "TX stop drains through the driver delay loop", stop_drains_async },
		{ "Stalled device quarantines and keeps every buffer", stall_quarantine },
		{ "Interleaved TX and RX traffic keeps FIFO order", interleaved_traffic },
		{ "Undefined data EQ events recover during traffic", unknown_eqe_recovery },
		{ "Link flap events update control during operation", link_flap },
	};

	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
