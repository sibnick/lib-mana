/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Tests for the asynchronous host device model in tests/host/device.c.
 * The fixture supplies control response payloads. The device model owns
 * all doorbell timing, EQ entries, and CQ entries.
 */
#include "host/test.h"
#include <stdlib.h>
#include "host/device.h"
#include "../src/internal.h"
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
	struct mana_data_queue txq, rxq;
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
		f->eq_disables = 0;
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

static int start_up(struct fixture *f)
{
	init_fixture(f);
	/* Note the data first. The device must answer the EQ self-test
	 * that mana_data_start runs before it returns.
	 */
	host_device_note_data(&f->dev, &f->data);
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_control_start(&f->control, &f->model.b, 0) == 0);
	TEST_CHECK(mana_data_start(&f->data, &f->control, 4096) == 0);
	return 0;
}

static int start_tx(struct fixture *f)
{
	TEST_CHECK(start_up(f) == 0);
	TEST_CHECK(mana_tx_start(&f->tx, &f->data, f->records, 8, 4096,
				 4096) == 0);
	host_device_note_queue(&f->dev, &f->tx.queue);
	return 0;
}

static int start_rx(struct fixture *f)
{
	TEST_CHECK(start_up(f) == 0);
	TEST_CHECK(mana_rx_start(&f->rx, &f->data, f->slots, 4, 4096,
				 4096) == 0);
	host_device_note_queue(&f->dev, &f->rx.queue);
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
	if (f->dev.bad_doorbells)
		fprintf(stderr, "bad doorbell off=%llx id=%llx head=%x\n",
			(unsigned long long)f->dev.last_bad_doorbell.offset,
			(unsigned long long)(f->dev.last_bad_doorbell.value & 0xffffff),
			(unsigned int)(f->dev.last_bad_doorbell.value >> 32));
	TEST_CHECK(f->dev.bad_doorbells == 0);
	TEST_CHECK(f->eq_disables == 1);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	return 0;
}

static int discard(struct fixture *f, size_t allocations)
{
	TEST_CHECK(mana_data_stop(&f->data) == -ENOTCONN);
	TEST_CHECK(mana_control_stop(&f->control) == -ENOTCONN);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	TEST_CHECK(f->model.host.live_allocations == allocations);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	/* Free only the simulated device, not the production quarantine. */
	TEST_CHECK(host_mock_cleanup(&f->model.host) == allocations);
	host_device_detach(&f->dev);
	return 0;
}

static void make_frame(unsigned char *frame, unsigned int length)
{
	unsigned int i;

	for (i = 0; i < length; i++)
		frame[i] = (unsigned char)(i + 1);
	frame[12] = 0x08;
	frame[13] = 0x00;
}

static int async_startup(void)
{
	struct fixture f;

	TEST_CHECK(start_up(&f) == 0);
	TEST_CHECK(f.model.b.ready && f.control.ready && f.data.ready);
	TEST_CHECK(f.dev.now_us > 0);
	TEST_CHECK(f.model.requests >= 5);
	TEST_CHECK(f.dev.work_count == 0);
	TEST_CHECK(f.dev.ctrl_pends == 0);
	return stop_down(&f);
}

static int doorbell_schedules_tx(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	/* The doorbell only recorded work. Nothing completed yet. */
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
	TEST_CHECK(f.dev.tx_frames == 0);
	host_device_tick(&f.dev, f.dev.data_latency_us - 1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
	TEST_CHECK(f.dev.tx_frames == 0);
	host_device_tick(&f.dev, 1);
	TEST_CHECK(f.dev.tx_frames == 1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0 && result.cookie == (void *)1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int tx_capture(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[42];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)2) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.length == 42 && result.wire_length == 60);
	/* The device read the padded wire image from the record DMA. */
	TEST_CHECK(f.dev.last_tx_length == 60);
	TEST_CHECK(!memcmp(f.dev.last_tx_frame, frame, sizeof(frame)));
	TEST_CHECK(f.dev.last_tx_frame[42] == 0);
	TEST_CHECK(result.type == 32 && result.vendor_error == 0);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int tx_batch(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];
	unsigned int i;

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	for (i = 0; i < 3; i++)
		TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame),
					  (void *)(uintptr_t)(i + 1)) == 1);
	TEST_CHECK(f.dev.tx_frames == 0);
	host_device_tick(&f.dev, 8);
	/* One due pass drains every WQE up to the last doorbell head. */
	TEST_CHECK(f.dev.tx_frames == 3);
	for (i = 0; i < 3; i++) {
		TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
		TEST_CHECK(result.cookie == (void *)(uintptr_t)(i + 1));
		TEST_CHECK(result.error == 0);
	}
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 0);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int late_completion(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	host_device_late(&f.dev, 3000);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 1000);
	TEST_CHECK(f.dev.tx_frames == 0);
	host_device_tick(&f.dev, 2000);
	TEST_CHECK(f.dev.tx_frames == 0);
	host_device_tick(&f.dev, 100);
	TEST_CHECK(f.dev.tx_frames == 1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int vendor_error(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	host_device_error_vendor(&f.dev, 0x123456);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0 && result.type == 32);
	TEST_CHECK(result.vendor_error == 0x123456);
	TEST_CHECK(!f.tx.quarantined && !f.data.quarantined);
	/* The queue stays usable for the next packet. */
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)2) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0 && result.cookie == (void *)2);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int duplicate_poisons(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	host_device_duplicate_tx(&f.dev);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 8);
	/* The device wrote two CQEs for one WQE. */
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == -EPROTO);
	TEST_CHECK(f.tx.quarantined && f.data.quarantined);
	/* The queue, region, and EQ DMA stay owned: 6 HWC + EQ + WQ + CQ. */
	return discard(&f, 9);
}

static int cqe_faults_poison(void)
{
	static const enum device_cqe_fault faults[] = {
		DEVICE_CQE_FAULT_BAD_ID,
		DEVICE_CQE_FAULT_BAD_CLIENT,
		DEVICE_CQE_FAULT_BAD_OWNER,
		DEVICE_CQE_FAULT_BAD_DIRECTION,
	};
	unsigned int i;

	for (i = 0; i < 4; i++) {
		struct fixture f;
		struct mana_tx_result result;
		unsigned char frame[60];

		make_frame(frame, sizeof(frame));
		TEST_CHECK(start_tx(&f) == 0);
		host_device_cqe_fault(&f.dev, faults[i]);
		TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame),
					  (void *)1) == 1);
		host_device_tick(&f.dev, 8);
		TEST_CHECK(mana_tx_poll(&f.tx, &result) < 0);
		TEST_CHECK(f.tx.quarantined && f.data.quarantined);
		/* The failed poll keeps the record DMA: 9 plus the record. */
		TEST_CHECK(discard(&f, 10) == 0);
	}
	return 0;
}

static unsigned int drain_calls;

static void drain_cb(void *ctx, const struct mana_tx_result *result)
{
	(void)result;
	(*(unsigned int *)ctx)++;
}

static int held_tx_times_out(void)
{
	struct fixture f;
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	host_device_hold_tx(&f.dev, true);
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 100);
	TEST_CHECK(f.dev.tx_frames == 0);
	/* The device never completes the held WQE, so the drain times out. */
	drain_calls = 0;
	TEST_CHECK(mana_tx_stop(&f.tx, drain_cb, &drain_calls) == -ETIMEDOUT);
	TEST_CHECK(drain_calls == 0);
	TEST_CHECK(f.tx.quarantined && f.data.quarantined);
	/* 9 plus the unreturned record DMA. */
	return discard(&f, 10);
}

static int rx_delivers(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	unsigned char frame[60];
	unsigned int i;

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_rx(&f) == 0);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_EMPTY);
	TEST_CHECK(host_device_rx_inject(&f.dev, frame, sizeof(frame)) == 0);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_EMPTY);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	TEST_CHECK(packet.length == 60);
	for (i = 0; i < 60; i++)
		TEST_CHECK(((unsigned char *)packet.dma.addr)[i] == frame[i]);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	TEST_CHECK(f.rx.delivered == 1 && f.rx.dropped == 0);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	host_device_forget_queue(&f.dev, &f.rx.queue);
	return stop_down(&f);
}

static int rx_drop_counts(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	unsigned char frame[60];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_rx(&f) == 0);
	host_device_rx_drop(&f.dev);
	TEST_CHECK(host_device_rx_inject(&f.dev, frame, sizeof(frame)) == 0);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_DROPPED);
	TEST_CHECK(packet.dma.addr == NULL);
	TEST_CHECK(f.rx.dropped == 1 && f.rx.delivered == 0);
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	host_device_forget_queue(&f.dev, &f.rx.queue);
	return stop_down(&f);
}

static int async_fence_stop(void)
{
	struct fixture f;
	struct mana_rx_packet packet = { 0 };
	unsigned char frame[100];

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_rx(&f) == 0);
	TEST_CHECK(host_device_rx_inject(&f.dev, frame, sizeof(frame)) == 0);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_rx_poll(&f.rx, &packet) == MANA_RX_PACKET);
	TEST_CHECK(packet.length == 100);
	TEST_CHECK(mana_rx_packet_release(&packet) == 0);
	/* Stop fences and destroys with no manual CQE from the test. */
	TEST_CHECK(mana_rx_stop(&f.rx) == 0);
	/* The library never deregisters a consumed region. */
	TEST_CHECK(f.object_destroys == 1 && f.region_destroys == 0);
	host_device_forget_queue(&f.dev, &f.rx.queue);
	return stop_down(&f);
}

static int link_events(void)
{
	struct fixture f;
	uint32_t generation;

	TEST_CHECK(start_up(&f) == 0);
	generation = f.model.b.link_generation;
	TEST_CHECK(host_device_bootstrap_event(&f.dev, 133, 3U << 24) == 0);
	host_device_tick(&f.dev, 10);
	TEST_CHECK(mana_bootstrap_poll(&f.model.b) == 0);
	TEST_CHECK(f.model.b.link_known && !f.model.b.link_up);
	TEST_CHECK(f.model.b.link_generation == generation + 1);
	TEST_CHECK(host_device_bootstrap_event(&f.dev, 133, 2U << 24) == 0);
	host_device_tick(&f.dev, 10);
	TEST_CHECK(mana_bootstrap_poll(&f.model.b) == 0);
	TEST_CHECK(f.model.b.link_up);
	TEST_CHECK(f.model.b.link_generation == generation + 2);
	return stop_down(&f);
}

static int unknown_eqe_ignored(void)
{
	struct fixture f;
	struct mana_tx_result result;
	unsigned char frame[60];
	static const uint32_t types[] = { 1, 63, 100, 177, 255 };
	uint32_t i;

	make_frame(frame, sizeof(frame));
	TEST_CHECK(start_tx(&f) == 0);
	for (i = 0; i < 5; i++) {
		TEST_CHECK(host_device_eqe(&f.dev, types[i], 0) == 0);
		host_device_tick(&f.dev, 10);
		TEST_CHECK(mana_data_poll(&f.data) == 0);
		TEST_CHECK(f.data.unknown_events == i + 1);
		TEST_CHECK(f.data.ready && !f.data.quarantined);
	}
	/* The datapath still completes after the ignored events. */
	TEST_CHECK(mana_tx_submit(&f.tx, frame, sizeof(frame), (void *)1) == 1);
	host_device_tick(&f.dev, 8);
	TEST_CHECK(mana_tx_poll(&f.tx, &result) == 1);
	TEST_CHECK(result.error == 0);
	TEST_CHECK(mana_tx_stop(&f.tx, NULL, NULL) == 0);
	host_device_forget_queue(&f.dev, &f.tx.queue);
	return stop_down(&f);
}

static int bad_doorbell_captured(void)
{
	struct fixture f;

	TEST_CHECK(start_up(&f) == 0);
	f.model.platform.write64(&f.model, &f.model.b.bar0, 0x1000,
				 UINT64_C(0x123) | (UINT64_C(32) << 32));
	TEST_CHECK(f.dev.bad_doorbells == 1);
	TEST_CHECK(f.dev.last_bad_doorbell.offset == 0x1000);
	TEST_CHECK((f.dev.last_bad_doorbell.value & 0xffffff) == 0x123);
	/* Clear the capture so the clean-stop check keeps its meaning. */
	f.dev.bad_doorbells = 0;
	/* The control plane still works after the stray doorbell. */
	return stop_down(&f);
}

int main(void)
{
	static const struct host_test tests[] = {
		{ "Attached model completes asynchronous startup", async_startup },
		{ "TX doorbell schedules work not completion", doorbell_schedules_tx },
		{ "TX completion carries the captured frame", tx_capture },
		{ "Three pending TX WQE entries complete in one pass", tx_batch },
		{ "Late TX completion waits past its deadline", late_completion },
		{ "Vendor error marks the TX result failed", vendor_error },
		{ "Duplicate TX completion poisons the queue", duplicate_poisons },
		{ "Corrupted CQE identifier fields poison the queue", cqe_faults_poison },
		{ "Held TX completion times out the drain", held_tx_times_out },
		{ "RX injection delivers one packet", rx_delivers },
		{ "Dropped RX indication counts as dropped", rx_drop_counts },
		{ "RX stop fences through the asynchronous device", async_fence_stop },
		{ "Bootstrap link events update the device state", link_events },
		{ "Undefined data EQ events stay ignored", unknown_eqe_ignored },
		{ "Unknown doorbell identifier is captured and ignored", bad_doorbell_captured },
	};

	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
