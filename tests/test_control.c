/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include <uk/mana/control.h>

#define PORT_HANDLE UINT64_C(0x0123456789abcdef)
#define TX_HANDLE UINT64_C(0x1234567800000010)
#define RX_HANDLE UINT64_C(0x1234567800000020)

enum mutation {
	NONE, ZERO_MTU, OLD_DEVICE, SMALL_MTU, HOSTMODE, NO_PORTS, NO_EQS,
	NO_SQ, NO_RQ, ZERO_MAC, MULTICAST_MAC, INVALID_PORT,
	BAD_DB, BAD_PD, BAD_KEY, BAD_SHORTFORM, INVALID_WQ, INVALID_CQ,
	DUPLICATE_CQ, INVALID_OBJECT, SHORT_LENGTH, FUTURE_VERSION, WRONG_DEVICE,
	UNCONFIGURED_QOS, INVALID_INDIRECTION, LARGE_INDIRECTION,
	HWC_WQ_COLLISION, HWC_CQ_COLLISION
};

struct fixture {
	struct host_bootstrap model;
	struct mana_control control;
	struct mana_control_queue tx, rx;
	struct mana_control_queue_spec spec;
	enum mutation mutation;
	uint32_t status_code, status;
	unsigned int commands, mismatch, registered, deregistered;
	unsigned int fence_commands, destroy_commands;
};

static void expected_header(unsigned char *out, uint32_t code,
			    uint32_t size, uint32_t response_size,
			    unsigned int request_version,
			    unsigned int response_version)
{
	mana_put32(out + 4, code);
	mana_put32(out + 8, request_version);
	mana_put32(out + 12, size);
	mana_put32(out + 20, code);
	mana_put32(out + 24, response_version);
	mana_put32(out + 28, response_size);
	mana_put32(out + 32, 0x10002);
}

static int respond(void *ctx, const unsigned char *req, size_t size,
		   unsigned char *resp, size_t capacity, size_t *length)
{
	struct fixture *f = ctx;
	unsigned char expected[MANA_GDMA_PAGE_SIZE] = { 0 };
	uint32_t code = mana_get32(req + 4);
	uint32_t in_size = 0, out_size = 0, req_version = 1, resp_version = 1;
	uint32_t type, i;
	if (code == 1 || code == 2 || code == 3 || code == 10)
		return -ENOTSUP;
	f->commands++;
	switch (code) {
	case 4:
		in_size = 40;
		out_size = 44;
		mana_put32(resp + 32, f->mutation == BAD_PD ? UINT32_MAX : 23);
		mana_put32(resp + 36, f->mutation == BAD_KEY ? UINT32_MAX : 0x91);
		mana_put32(resp + 40, f->mutation == BAD_DB ? UINT32_MAX : 0);
		f->registered++;
		break;
	case 5:
		in_size = 40;
		out_size = 32;
		f->deregistered++;
		break;
	case 0x20001:
		in_size = 88;
		out_size = 80;
		resp_version = 3;
		mana_put32(expected + 76, 1);
		mana_put32(expected + 80, 1);
		mana_put64(resp + 32, UINT64_C(0xfedcba9876543210));
		mana_put64(resp + 40, UINT64_C(0x1234000000000000));
		mana_put64(resp + 48, 4);
		mana_put64(resp + 56, 8);
		mana_put16(resp + 64, f->mutation == NO_PORTS ? 0 : 1);
		resp[66] = f->mutation == HOSTMODE;
		mana_put32(resp + 68, f->mutation == NO_EQS ? 0 : 4);
		mana_put16(resp + 72, f->mutation == SMALL_MTU ? 81 :
			   (f->mutation == ZERO_MTU ? 0 : 9014));
		if (f->mutation == OLD_DEVICE)
			mana_put16(resp + 8, 1);
		break;
	case 0x20008:
		in_size = 44;
		out_size = 64;
		mana_put32(resp + 32, f->mutation == NO_SQ ? 0 : 2);
		mana_put32(resp + 36, f->mutation == NO_RQ ? 0 : 2);
		mana_put32(resp + 40, f->mutation == INVALID_INDIRECTION ? 3 :
			   (f->mutation == LARGE_INDIRECTION ? 512 : 64));
		memcpy(resp + 48, "\x02\x11\x22\x33\x44\x55", 6);
		if (f->mutation == ZERO_MAC)
			memset(resp + 48, 0, 6);
		if (f->mutation == MULTICAST_MAC)
			resp[48] = 1;
		mana_put64(resp + 56, f->mutation == INVALID_PORT ?
			   UINT64_MAX : PORT_HANDLE);
		break;
	case 0x20003:
		in_size = 56;
		out_size = 36;
		mana_put64(expected + 40, PORT_HANDLE);
		mana_put32(expected + 48, 23);
		mana_put32(expected + 52, 0);
		mana_put16(resp + 32, 0x1234);
		resp[34] = f->mutation == BAD_SHORTFORM ? 2 : 1;
		break;
	case 0x20007:
		in_size = 120 + f->control.indirection_entries * 8;
		out_size = 40;
		req_version = 5;
		resp_version = 2;
		mana_put64(expected + 40, PORT_HANDLE);
		mana_put16(expected + 48, (uint16_t)f->control.indirection_entries);
		mana_put16(expected + 50, 120);
		/* Enable is the only caller-selected field in this golden request. */
		mana_put32(expected + 52, mana_get32(req + 52));
		expected[60] = (unsigned char)mana_get32(req + 52);
		expected[62] = expected[60];
		mana_put64(expected + 64, expected[60] ? RX_HANDLE : UINT64_MAX);
		mana_put16(expected + 116, 0x3f);
		if (expected[60])
			for (i = 0; i < f->control.indirection_entries; i++)
				mana_put64(expected + 120 + i * 8, RX_HANDLE);
		break;
	case 0x2000a:
		in_size = 48;
		out_size = 48;
		resp_version = 2;
		mana_put64(expected + 40, PORT_HANDLE);
		mana_put32(resp + 32, 50000);
		resp[36] = f->mutation == UNCONFIGURED_QOS;
		mana_put32(resp + 40, 25000);
		break;
	case 0x20004:
		in_size = 96;
		out_size = 56;
		req_version = 3;
		resp_version = 2;
		type = mana_get32(req + 48);
		if (type != MANA_CONTROL_TX && type != MANA_CONTROL_RX)
			f->mismatch = 1;
		mana_put64(expected + 40, PORT_HANDLE);
		mana_put32(expected + 48, type);
		mana_put64(expected + 56, f->spec.wq_region);
		mana_put64(expected + 64, f->spec.cq_region);
		mana_put32(expected + 72, f->spec.wq_size);
		mana_put32(expected + 76, f->spec.cq_size);
		mana_put32(expected + 80, f->spec.moderation_context);
		mana_put32(expected + 84, f->spec.parent_eq);
		mana_put32(resp + 32, f->mutation == INVALID_WQ ? 0x1000000 : 21);
		if (f->mutation == HWC_WQ_COLLISION)
			mana_put32(resp + 32, type == MANA_CONTROL_TX ? 7 : 6);
		mana_put32(resp + 36, f->mutation == INVALID_CQ ? 32 :
			   (type == MANA_CONTROL_TX || f->mutation == DUPLICATE_CQ ? 10 : 11));
		if (f->mutation == HWC_CQ_COLLISION)
			mana_put32(resp + 36, 5);
		mana_put64(resp + 40, f->mutation == INVALID_OBJECT ? UINT64_MAX :
			   (type == MANA_CONTROL_TX ? TX_HANDLE : RX_HANDLE));
		if (f->mutation == SHORT_LENGTH)
			*length = 32;
		if (f->mutation == FUTURE_VERSION)
			mana_put16(resp + 8, 3);
		if (f->mutation == WRONG_DEVICE)
			mana_put32(resp + 16, 2);
		break;
	case 0x20005:
		in_size = 56;
		out_size = 32;
		type = mana_get32(req + 40);
		mana_put32(expected + 40, type);
		mana_put64(expected + 48, type == MANA_CONTROL_TX ? TX_HANDLE : RX_HANDLE);
		f->destroy_commands++;
		break;
	case 0x20006:
		in_size = 48;
		out_size = 32;
		mana_put64(expected + 40, RX_HANDLE);
		f->fence_commands++;
		break;
	default:
		f->mismatch = 1;
		return -ENOTSUP;
	}
	expected_header(expected, code, in_size, out_size, req_version, resp_version);
	if (size != in_size || capacity < out_size ||
	    mana_get32(req + 28) != out_size || memcmp(req, expected, size))
		f->mismatch = 1;
	if (f->status_code == code)
		mana_put32(resp + 24, f->status);
	return 0;
}

static void init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	f->model.respond = respond;
	f->model.respond_ctx = f;
	f->spec = (struct mana_control_queue_spec) {
		.wq_region = UINT64_C(0xabcdef0123456789),
		.cq_region = UINT64_C(0x876543210fedcba9),
		.wq_size = 8192, .cq_size = 4096,
		.parent_eq = 17, .moderation_context = 0
	};
}

static int start(struct fixture *f)
{
	int err = mana_bootstrap_start(&f->model.b, &f->model.platform);
	return err ? err : mana_control_start(&f->control, &f->model.b, 0);
}

static int stop(struct fixture *f)
{
	TEST_CHECK(mana_control_stop(&f->control) == 0);
	TEST_CHECK(!f->control.bootstrap && !f->model.b.control_owned);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	return 0;
}

static int discard_quarantined_model(struct fixture *f)
{
	TEST_CHECK(f->control.quarantined && !f->control.ready);
	TEST_CHECK(mana_control_stop(&f->control) == -ENOTCONN);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	TEST_CHECK(f->model.host.live_allocations == 6 && f->model.irq_live);
	TEST_CHECK(!f->model.mismatch && !f->mismatch);
	/* Dispose only this simulated device. Production retains these pages. */
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 6);
	return 0;
}

static int fence_poll(void *ctx)
{
	struct mana_control_queue *q = ctx;
	return mana_control_fence_complete(q);
}

static int empty_poll(void *ctx)
{
	(void)ctx;
	return 0;
}

static int failing_poll(void *ctx)
{
	(void)ctx;
	return -EIO;
}

static int discovery(void)
{
	struct fixture f;
	struct mana_control other = { 0 };
	init(&f);
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(f.commands == 6 && f.control.ready && f.control.registered);
	TEST_CHECK(f.control.pd_id == 23 && f.control.mkey == 0x91);
	TEST_CHECK(f.model.b.pd_id == 3 && f.control.doorbell_id == 0);
	TEST_CHECK(f.control.capabilities[0] == UINT64_C(0xfedcba9876543210));
	TEST_CHECK(f.control.capabilities[1] == UINT64_C(0x1234000000000000));
	TEST_CHECK(f.control.capabilities[2] == 4 && f.control.capabilities[3] == 8);
	TEST_CHECK(f.control.max_ports == 1 && f.control.max_eqs == 4);
	TEST_CHECK(f.control.max_sq == 2 && f.control.max_rq == 2);
	TEST_CHECK(f.control.indirection_entries == 64);
	TEST_CHECK(f.control.frame_mtu == 9014 && f.control.mtu == 1500);
	TEST_CHECK(f.control.tx_vport_offset == 0x1234 && f.control.short_form_allowed);
	TEST_CHECK(!memcmp(f.control.mac, "\x02\x11\x22\x33\x44\x55", 6));
	TEST_CHECK(f.control.vport == PORT_HANDLE);
	TEST_CHECK(f.control.link_speed_mbps == 25000 && f.control.qos_speed_mbps == 50000);
	TEST_CHECK(!f.model.b.link_known);
	TEST_CHECK(mana_control_start(&other, &f.model.b, 0) == -EBUSY);
	TEST_CHECK(mana_bootstrap_stop(&f.model.b) == -EBUSY && f.model.b.ready);
	TEST_CHECK(mana_control_set_mtu(&f.control, 67) == -EINVAL);
	TEST_CHECK(mana_control_set_mtu(&f.control, 9001) == -EINVAL);
	TEST_CHECK(mana_control_set_mtu(&f.control, 9000) == 0);
	TEST_CHECK(f.control.mtu == 9000);
	TEST_CHECK(stop(&f) == 0);
	TEST_CHECK(f.deregistered == 1);
	return 0;
}

static int queues(void)
{
	struct fixture f;
	struct mana_control_queue extra = { 0 };
	unsigned int commands;
	init(&f);
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == -EINVAL);
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == 0);
	TEST_CHECK(f.tx.live && f.tx.handle == TX_HANDLE && f.tx.wq_id == 21 && f.tx.cq_id == 10);
	TEST_CHECK(f.tx.spec.wq_region == f.spec.wq_region);
	TEST_CHECK(mana_control_create_queue(&f.control, &extra, MANA_CONTROL_TX, &f.spec) == -ENOSPC);
	TEST_CHECK(mana_control_create_queue(&f.control, &f.rx, MANA_CONTROL_RX, &f.spec) == -EINVAL);
	f.spec.wq_region++;
	f.spec.cq_region++;
	TEST_CHECK(mana_control_create_queue(&f.control, &f.rx, MANA_CONTROL_RX, &f.spec) == 0);
	TEST_CHECK(f.rx.live && f.rx.handle == RX_HANDLE && f.rx.cq_id == 11);
	TEST_CHECK(mana_control_set_mtu(&f.control, 1500) == -EBUSY);
	TEST_CHECK(mana_control_stop(&f.control) == -EBUSY);
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == 0 && f.control.rx_enabled);
	commands = f.commands;
	TEST_CHECK(mana_control_destroy_queue(&f.rx) == -EBUSY);
	TEST_CHECK(mana_control_fence_rx(&f.rx) == -EINVAL);
	TEST_CHECK(f.commands == commands);
	TEST_CHECK(mana_control_set_rx(&f.control, 0) == 0);
	TEST_CHECK(mana_control_destroy_queue(&f.rx) == -EBUSY);
	TEST_CHECK(mana_control_fence_rx(&f.rx) == 0);
	TEST_CHECK(f.rx.fence_pending && !f.rx.fenced);
	TEST_CHECK(mana_control_destroy_queue(&f.rx) == -EBUSY);
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == -EINVAL);
	TEST_CHECK(mana_control_wait_fence(&f.rx, fence_poll, &f.rx) == 0);
	TEST_CHECK(f.rx.fenced && !f.rx.fence_pending);
	TEST_CHECK(mana_control_fence_complete(&f.rx) == -EPROTO);
	TEST_CHECK(mana_control_destroy_queue(&f.rx) == 0 && !f.rx.control);
	TEST_CHECK(mana_control_destroy_queue(&f.tx) == 0 && !f.tx.control);
	TEST_CHECK(f.fence_commands == 1 && f.destroy_commands == 2);
	return stop(&f);
}

static int rejected_specs(void)
{
	struct fixture f;
	struct mana_control_queue_spec bad;
	unsigned int commands, i;
	init(&f);
	TEST_CHECK(start(&f) == 0);
	commands = f.commands;
	for (i = 0; i < 9; i++) {
		bad = f.spec;
		switch (i) {
		case 0: bad.wq_region = 0; break;
		case 1: bad.cq_region = 0; break;
		case 2: bad.wq_region = bad.cq_region; break;
		case 3: bad.wq_size = 2048; break;
		case 4: bad.wq_size = 6144; break;
		case 5: bad.cq_size = 2048; break;
		case 6: bad.cq_size = 12288; break;
		case 7: bad.parent_eq = 0x1000000; break;
		default: bad.parent_eq = f.model.b.eq_id; break;
		}
		TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &bad) == -EINVAL);
		TEST_CHECK(!f.tx.control && !f.control.tx);
	}
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, 0, &f.spec) == -EINVAL);
	TEST_CHECK(f.commands == commands);
	return stop(&f);
}

static int old_firmware(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 0; i < 2; i++) {
		init(&f);
		f.mutation = i ? OLD_DEVICE : ZERO_MTU;
		f.status_code = 0x2000a;
		f.status = UINT32_MAX;
		TEST_CHECK(start(&f) == 0);
		TEST_CHECK(f.control.frame_mtu == 1514 && f.control.mtu == 1500);
		TEST_CHECK(f.control.link_query_error == -EOPNOTSUPP);
		TEST_CHECK(!f.control.link_speed_mbps && !f.control.qos_speed_mbps);
		TEST_CHECK(f.model.errors >= 2);
		TEST_CHECK(mana_control_set_mtu(&f.control, 1501) == -EINVAL);
		TEST_CHECK(stop(&f) == 0);
	}
	init(&f);
	f.mutation = INVALID_INDIRECTION;
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(f.control.indirection_entries == 64 && f.model.errors);
	TEST_CHECK(stop(&f) == 0);
	init(&f);
	f.mutation = LARGE_INDIRECTION;
	TEST_CHECK(start(&f) == -E2BIG);
	TEST_CHECK(f.control.registered && !f.control.quarantined);
	TEST_CHECK(stop(&f) == 0);
	return 0;
}

static int bad_discovery(void)
{
	static const enum mutation mutations[] = {
		SMALL_MTU, HOSTMODE, NO_PORTS, NO_EQS, NO_SQ, NO_RQ, ZERO_MAC,
		MULTICAST_MAC, INVALID_PORT, BAD_DB, BAD_PD, BAD_KEY, UNCONFIGURED_QOS
	};
	struct fixture f;
	unsigned int i;
	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		init(&f);
		f.mutation = mutations[i];
		TEST_CHECK(start(&f) == (mutations[i] == UNCONFIGURED_QOS ? -EINVAL : -EPROTO));
		TEST_CHECK(!f.control.ready && f.control.registered);
		TEST_CHECK(!f.control.quarantined && f.model.errors);
		f.mutation = NONE;
		TEST_CHECK(stop(&f) == 0);
	}
	init(&f);
	TEST_CHECK(mana_bootstrap_start(&f.model.b, &f.model.platform) == 0);
	TEST_CHECK(mana_control_start(&f.control, &f.model.b, 1) == -EINVAL);
	TEST_CHECK(stop(&f) == 0);
	return 0;
}

static int failed_start_commands(void)
{
	static const uint32_t codes[] = { 4, 0x20001, 0x20008, 0x20003, 0x20007, 0x2000a };
	struct fixture f;
	unsigned int i;
	for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		init(&f);
		f.status_code = codes[i];
		f.status = 0x1234;
		TEST_CHECK(start(&f) == -EPROTO && !f.control.ready);
		TEST_CHECK(f.control.registered == (codes[i] != 4));
		TEST_CHECK(!f.control.quarantined && !f.model.b.error);
		f.status = 0;
		TEST_CHECK(stop(&f) == 0);
		TEST_CHECK(f.deregistered == (codes[i] != 4));
	}
	return 0;
}

static int firmware_retry(void)
{
	struct fixture f;
	init(&f);
	TEST_CHECK(start(&f) == 0);
	f.status_code = 0x20004;
	f.status = UINT32_MAX;
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == -EOPNOTSUPP);
	TEST_CHECK(!f.tx.control && !f.control.tx && !f.control.quarantined);
	f.status = 0;
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == 0);
	f.status_code = 0x20005;
	f.status = 1;
	TEST_CHECK(mana_control_destroy_queue(&f.tx) == -EPROTO);
	TEST_CHECK(f.tx.live && f.control.tx == &f.tx && !f.tx.quarantined);
	f.status = 0;
	TEST_CHECK(mana_control_destroy_queue(&f.tx) == 0);
	f.spec.wq_region++;
	f.spec.cq_region++;
	TEST_CHECK(mana_control_create_queue(&f.control, &f.rx, MANA_CONTROL_RX, &f.spec) == 0);
	f.status_code = 0x20007;
	f.status = 1;
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == -EPROTO && !f.control.rx_enabled);
	f.status = 0;
	TEST_CHECK(mana_control_set_rx(&f.control, 1) == 0);
	f.status = 1;
	TEST_CHECK(mana_control_set_rx(&f.control, 0) == -EPROTO && f.control.rx_enabled);
	f.status = 0;
	TEST_CHECK(mana_control_set_rx(&f.control, 0) == 0);
	f.status_code = 0x20006;
	f.status = 1;
	TEST_CHECK(mana_control_fence_rx(&f.rx) == -EPROTO && !f.rx.fence_pending);
	f.status = 0;
	TEST_CHECK(mana_control_fence_rx(&f.rx) == 0);
	TEST_CHECK(mana_control_wait_fence(&f.rx, fence_poll, &f.rx) == 0);
	TEST_CHECK(mana_control_destroy_queue(&f.rx) == 0);
	f.status_code = 5;
	f.status = 1;
	TEST_CHECK(mana_control_stop(&f.control) == -EPROTO);
	TEST_CHECK(f.control.registered && f.model.b.control_owned);
	f.status = 0;
	return stop(&f);
}

static int uncertain_create(void)
{
	static const enum mutation mutations[] = {
		INVALID_WQ, INVALID_CQ, INVALID_OBJECT, SHORT_LENGTH, FUTURE_VERSION,
		WRONG_DEVICE, HWC_WQ_COLLISION, HWC_CQ_COLLISION
	};
	struct fixture f;
	unsigned int i;
	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		init(&f);
		TEST_CHECK(start(&f) == 0);
		f.mutation = mutations[i];
		TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == -EPROTO);
		TEST_CHECK(f.tx.control == &f.control && f.tx.quarantined);
		TEST_CHECK(f.tx.spec.wq_region == f.spec.wq_region);
		TEST_CHECK(mana_control_destroy_queue(&f.tx) == -ENOTCONN);
		TEST_CHECK(discard_quarantined_model(&f) == 0);
	}
	init(&f);
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == 0);
	f.mutation = DUPLICATE_CQ;
	f.spec.wq_region++;
	f.spec.cq_region++;
	TEST_CHECK(mana_control_create_queue(&f.control, &f.rx, MANA_CONTROL_RX, &f.spec) == -EPROTO);
	TEST_CHECK(f.tx.quarantined && f.rx.quarantined);
	return discard_quarantined_model(&f);
}

static int transport_failures(void)
{
	static const enum host_bootstrap_fault faults[] = {
		HOST_BOOTSTRAP_NO_TX, HOST_BOOTSTRAP_BAD_MESSAGE_ID,
		HOST_BOOTSTRAP_WRONG_QUEUE, HOST_BOOTSTRAP_BAD_LENGTH,
		HOST_BOOTSTRAP_SHORT_RESPONSE
	};
	struct fixture f;
	unsigned int i;
	int err;
	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		init(&f);
		TEST_CHECK(start(&f) == 0);
		f.model.fault = faults[i];
		err = mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec);
		TEST_CHECK(err == (faults[i] == HOST_BOOTSTRAP_NO_TX ? -ETIMEDOUT : -EPROTO));
		TEST_CHECK(f.model.b.pending && f.tx.quarantined);
		if (faults[i] == HOST_BOOTSTRAP_NO_TX)
			TEST_CHECK(f.model.host.now_us == UINT64_C(30000000));
		TEST_CHECK(discard_quarantined_model(&f) == 0);
	}
	init(&f);
	TEST_CHECK(start(&f) == 0);
	TEST_CHECK(mana_control_create_queue(&f.control, &f.tx, MANA_CONTROL_TX, &f.spec) == 0);
	f.model.fault = HOST_BOOTSTRAP_NO_TX;
	TEST_CHECK(mana_control_destroy_queue(&f.tx) == -ETIMEDOUT);
	TEST_CHECK(f.tx.live && f.tx.quarantined && f.tx.handle == TX_HANDLE);
	return discard_quarantined_model(&f);
}

static int fence_bounds(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 0; i < 3; i++) {
		init(&f);
		TEST_CHECK(start(&f) == 0);
		TEST_CHECK(mana_control_create_queue(&f.control, &f.rx, MANA_CONTROL_RX, &f.spec) == 0);
		TEST_CHECK(mana_control_fence_rx(&f.rx) == 0);
		if (i == 2)
			f.model.clock_fail = 1;
		TEST_CHECK(mana_control_wait_fence(&f.rx, i == 1 ? failing_poll : empty_poll, NULL) ==
			   (i == 0 ? -ETIMEDOUT : -EIO));
		TEST_CHECK(f.rx.fence_pending && !f.rx.fenced && f.rx.quarantined);
		if (i == 0)
			TEST_CHECK(f.model.host.now_us == UINT64_C(10000000));
		TEST_CHECK(discard_quarantined_model(&f) == 0);
	}
	return 0;
}

static void event(struct fixture *f, uint32_t type, uint32_t data)
{
	unsigned char *eq = f->model.b.pages[MANA_HWC_EQ].addr;
	uint32_t producer = f->model.eq_producer++;
	unsigned char *entry = eq + (producer % 256) * 16;
	memset(entry, 0, 16);
	mana_put32(entry, data);
	mana_put32(entry + 12, type | ((producer / 256) & 7) << 29);
	f->model.handler(f->model.arg);
}

static int async_events(void)
{
	struct fixture f;
	unsigned int i;
	static const uint32_t errors[] = { 132, 134, 135, 255 };
	init(&f);
	TEST_CHECK(start(&f) == 0);
	event(&f, 133, (2U << 24) | 99);
	TEST_CHECK(mana_control_poll(&f.control) == 0);
	TEST_CHECK(f.model.b.link_known && f.model.b.link_up && f.model.b.link_generation == 1);
	event(&f, 133, 3U << 24);
	TEST_CHECK(mana_control_poll(&f.control) == 0);
	TEST_CHECK(!f.model.b.link_up && f.model.b.link_generation == 2);
	event(&f, 133, (1U << 24) | 0xffffff);
	TEST_CHECK(mana_control_poll(&f.control) == 0);
	TEST_CHECK(f.model.b.link_generation == 2);
	TEST_CHECK(stop(&f) == 0);
	for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		init(&f);
		TEST_CHECK(start(&f) == 0);
		event(&f, errors[i], 0);
		TEST_CHECK(mana_control_poll(&f.control) == (errors[i] == 255 ? -EPROTO : -ENOTSUP));
		TEST_CHECK(discard_quarantined_model(&f) == 0);
	}
	init(&f);
	TEST_CHECK(start(&f) == 0);
	event(&f, 133, 4U << 24);
	TEST_CHECK(mana_control_poll(&f.control) == -ENOTSUP);
	return discard_quarantined_model(&f);
}

static int failed_tx_configuration(void)
{
	struct fixture f;
	init(&f);
	f.mutation = BAD_SHORTFORM;
	TEST_CHECK(start(&f) == -EPROTO);
	TEST_CHECK(f.control.registered && f.control.configured);
	return discard_quarantined_model(&f);
}

int main(void)
{
	static const struct host_test cases[] = {
		{ "Device, MAC, frame MTU, link speed, and registered PD", discovery },
		{ "Exact TX/RX queue, steering, fence, and destroy wire shapes", queues },
		{ "Invalid queue specifications do not send commands", rejected_specs },
		{ "Source-defined old firmware MTU and unsupported link", old_firmware },
		{ "Malformed device and port fields", bad_discovery },
		{ "Each start command failure retains explicit ownership", failed_start_commands },
		{ "Firmware failures retain objects and permit bounded retry", firmware_retry },
		{ "Uncertain create results quarantine DMA regions", uncertain_create },
		{ "Transport failures retain in-flight and queue ownership", transport_failures },
		{ "RX fence completion and clock bounds", fence_bounds },
		{ "Async link, timeout, reset, service, and unknown events", async_events },
		{ "Invalid TX response retains registration", failed_tx_configuration },
	};
	return host_test_run(cases, sizeof(cases) / sizeof(cases[0]));
}
