/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"

static int success(void)
{
	struct host_bootstrap f;
	unsigned char req[40] = { 0 }, resp[72];
	size_t length;
	unsigned int i;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	/* Also exercise high page-frame nibbles in the SMC message. */
	f.host.next_device_address = UINT64_C(0x1234567890000000);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.ready && f.b.mana_device_id == 0x10002);
	/* Init fields 10 and 11 reach the context and the SQ WQE destination. */
	TEST_CHECK(f.b.pf_dest_vrq_id == HOST_BOOTSTRAP_INIT_PF_DEST_RQ_ID);
	TEST_CHECK(f.b.pf_dest_vrcq_id == HOST_BOOTSTRAP_INIT_PF_DEST_CQ_ID);
	TEST_CHECK(f.requests == 4 && !f.mismatch && f.b.limits.msix == 32);
	TEST_CHECK(f.host.live_allocations == 6 && f.irq_live);
	/* One initial and one re-arming RQ post per completed request.
	 * One CQ arm per request. One EQ arm for init plus one per request.
	 */
	TEST_CHECK(f.rq_bells == 5 && f.cq_bells == 4 && f.eq_bells == 5);
	mana_put32(req + 4, 2);
	mana_put32(req + 8, 1);
	mana_put32(req + 12, 40);
	mana_put32(req + 20, 2);
	mana_put32(req + 24, 1);
	mana_put32(req + 28, 72);
	/* Wrap SQ, RQ, CQ, EQ and all three owner bits. */
	for (i = 0; i < 2200; i++)
		TEST_CHECK(mana_hwc_request(&f.b, req, 40, resp, 72, &length) == 0);
	TEST_CHECK(length == 72 && !f.mismatch);
	TEST_CHECK(f.rq_bells == 2205 && f.cq_bells == 2204 && f.eq_bells == 2205);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.b.ready && !f.irq_live && !f.maps);
	TEST_CHECK(f.host.live_allocations == 0 && f.smc_destroy == 1);
	TEST_CHECK(host_bootstrap_config(&f, 4, 2) == 0 && host_bootstrap_config(&f, 0x42, 2) == 0);
	TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	f.wide_bar = 1;
	mana_put32(f.config + 0x10, 4);
	mana_put32(f.config + 0x14, 1);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.bars[0].physical == UINT64_C(0x100000000));
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(host_bootstrap_config(&f, 0x10, 4) == 4 && host_bootstrap_config(&f, 0x14, 4) == 1);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	mana_put32(f.config + 0x10, UINT32_C(0x80000008));
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int invalid_resources(void)
{
	struct host_bootstrap f;
	unsigned int i;
	for (i = 0; i < 19; i++) {
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		switch (i) {
		case 0: f.config[2] = 0xb9; break;
		case 1: f.platform.dma_guarantees = MANA_DMA_COHERENT; break;
		case 2: f.config[4] = 4; break;
		case 3: mana_put32(f.host.bar + 0x10, 2048); break;
		case 4: mana_put64(f.host.bar + 8, 8192); break;
		case 5: mana_put64(f.host.bar + 0x18, 8190); break;
		case 6: mana_put64(f.host.bar + 0x18, 4096); break;
		case 7: f.config[6] = 0; break;
		case 8: f.config[0x40] = 1; f.config[0x41] = 0x40; break;
		case 9: mana_put32(f.config + 0x44, 0x1ff8); break;
		case 10: mana_put32(f.config + 0x48, 0x400); break;
		case 11: f.map_fail = 1; break;
		case 12: f.irq_fail = 1; break;
		case 13: mana_put32(f.config + 0x10, UINT32_C(0x80000001)); break;
		case 14: mana_put32(f.config + 0x10, UINT32_C(0x80000002)); break;
		case 15: mana_put32(f.config + 0x44, 0x100); break;
		case 16: mana_put32(f.config + 0x48, 0x18); break;
		case 17:
			mana_put32(f.config + 0x40, 0x10011);
			mana_put32(f.host.bar + 0x41c, 0);
			break;
		case 18: mana_put64(f.host.bar + 0x18, UINT64_MAX - 3); break;
		}
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(!f.b.ready && !f.b.quarantined);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	/* An already-enabled MSI-X capability must not be taken over. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	f.config[0x43] = 0x80; /* MSI-X Message Control: Enable already set. */
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EBUSY);
	TEST_CHECK(f.errors && !f.b.ready && !f.b.quarantined);
	TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
	TEST_CHECK(host_bootstrap_config(&f, 4, 2) == 0);
	TEST_CHECK(host_bootstrap_config(&f, 0x42, 2) == 0x8000);
	TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	/* BAR5 cannot be 64-bit: the high half would leave configuration space. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	mana_put32(f.config + 0x24, 4); /* BAR5 reports a 64-bit memory BAR. */
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EPROTO);
	TEST_CHECK(f.errors && !f.b.ready && !f.b.quarantined);
	TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
	TEST_CHECK(host_bootstrap_config(&f, 4, 2) == 0);
	TEST_CHECK(host_bootstrap_config(&f, 0x24, 4) == 4);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int allocations(void)
{
	struct host_bootstrap f;
	unsigned int i;
	for (i = 1; i <= 6; i++) {
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		TEST_CHECK(host_fault_arm(&f.host, HOST_FAULT_ALLOC, i, -ENOMEM) == 0);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -ENOMEM);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(!f.smc_destroy && !f.b.quarantined);
		TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		f.dma_bad_at = i;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EPROTO);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	return 0;
}

static int protocol_errors(void)
{
	struct host_bootstrap f;
	enum host_bootstrap_fault faults[] = {
		HOST_BOOTSTRAP_SMC_STATUS, HOST_BOOTSTRAP_NO_INIT, HOST_BOOTSTRAP_BAD_OWNER, HOST_BOOTSTRAP_DUP_INIT, HOST_BOOTSTRAP_SMALL_REQUEST,
		HOST_BOOTSTRAP_BAD_DB, HOST_BOOTSTRAP_BAD_CQ, HOST_BOOTSTRAP_BAD_LENGTH, HOST_BOOTSTRAP_BAD_MESSAGE_ID, HOST_BOOTSTRAP_BAD_STATUS,
		HOST_BOOTSTRAP_NO_TX, HOST_BOOTSTRAP_NO_IRQ, HOST_BOOTSTRAP_NO_MANA, HOST_BOOTSTRAP_BAD_VERSION,
		HOST_BOOTSTRAP_UNKNOWN_EVENT, HOST_BOOTSTRAP_MISSING_INIT, HOST_BOOTSTRAP_WRONG_QUEUE, HOST_BOOTSTRAP_SHORT_RESPONSE, HOST_BOOTSTRAP_BAD_SMC_TYPE,
	};
	unsigned int i;
	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		host_bootstrap_init(&f, faults[i]);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(f.smc_destroy == 1 && !f.b.quarantined && !f.b.ready);
		TEST_CHECK(f.host.now_us <= UINT64_C(60000000));
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
		host_bootstrap_init(&f, HOST_BOOTSTRAP_SMC_BUSY);
		f.clock_fail = 1;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EIO);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	host_bootstrap_init(&f, HOST_BOOTSTRAP_SMC_BUSY);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -ETIMEDOUT);
	TEST_CHECK(f.host.now_us == UINT64_C(20000000));
	TEST_CHECK(!f.host.live_allocations && !f.smc_destroy);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	f.irq_free_fail = 1;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == -EIO);
	TEST_CHECK(f.b.quarantined && f.host.live_allocations == 6 && f.irq_live);
	f.irq_free_fail = 0;
	f.unmap_fail = 1;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == -EIO);
	TEST_CHECK(f.b.quarantined && f.host.live_allocations == 6 && !f.irq_live);
	f.unmap_fail = 0;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int quarantine(void)
{
	struct host_bootstrap f;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	f.fault = HOST_BOOTSTRAP_DESTROY_TIMEOUT;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == -ETIMEDOUT);
	TEST_CHECK(f.b.quarantined && !f.b.ready);
	TEST_CHECK(f.host.live_allocations == 6 && f.maps == 1 && f.irq_live);
	TEST_CHECK(f.host.now_us == UINT64_C(20000000));
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EBUSY);
	f.fault = HOST_BOOTSTRAP_NORMAL;
	mana_put32(f.host.bar + 0x11c, 0x82);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live && f.smc_destroy == 1);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int config_failures(void)
{
	struct host_bootstrap f;
	unsigned int count, i;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	count = f.cfg_calls;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	for (i = 1; i <= count; i++) {
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		f.cfg_fail_at = i;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.b.ready);
		if (f.b.quarantined)
			TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(host_bootstrap_config(&f, 0x10, 4) == UINT32_C(0x80000000));
		TEST_CHECK(host_bootstrap_config(&f, 4, 2) == 0);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	/* After a confirmed destroy, retry PCI cleanup without replaying SMC. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	count = f.cfg_calls;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	count = f.cfg_calls - count;
	for (i = 1; i <= count; i++) {
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
		f.cfg_fail_at = f.cfg_calls + i;
		TEST_CHECK(mana_bootstrap_stop(&f.b) == -EIO);
		TEST_CHECK(f.b.quarantined && f.host.live_allocations == 6);
		TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
		TEST_CHECK(f.smc_destroy == 1 && !f.maps && !f.irq_live);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	return 0;
}

struct response_context {
	unsigned int calls, handled;
	size_t length;
	uint32_t status;
};

static int limits_response(void *ctx, const unsigned char *req, size_t req_length,
			   unsigned char *resp, size_t capacity, size_t *length)
{
	struct response_context *r = ctx;
	unsigned int i;
	r->calls++;
	if (mana_get32(req + 4) != 2 || mana_get32(req + 32) ||
	    mana_get32(req + 36))
		return -ENOTSUP;
	if (req_length != 40 || capacity != 4096 || *length != 72 ||
	    mana_get32(resp + 4) != 2 || mana_get32(resp + 8) != 1 ||
	    mana_get32(resp + 12) != 72 || mana_get32(resp + 16) ||
	    mana_get32(resp + 20) || mana_get32(resp + 24))
		return -EINVAL;
	for (i = 1; i < 10; i++)
		mana_put32(resp + 32 + i * 4, 7);
	mana_put32(resp + 24, r->status);
	*length = r->length;
	r->handled++;
	return 0;
}

static int response_extension(void)
{
	struct host_bootstrap f;
	struct response_context r = {0, 0, 72, 0};
	unsigned char req[40] = {0}, resp[72];
	size_t length = 0;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	f.respond = limits_response;
	f.respond_ctx = &r;
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(r.calls == 4 && r.handled == 1);
	TEST_CHECK(f.b.limits.sq == 7 && f.b.limits.msix == 7 && !f.mismatch);
	mana_put32(req + 4, 2);
	mana_put32(req + 8, 1);
	mana_put32(req + 12, 40);
	mana_put32(req + 20, 2);
	mana_put32(req + 24, 1);
	mana_put32(req + 28, 72);
	r.length = 64;
	memset(resp, 0xa5, sizeof(resp));
	TEST_CHECK(mana_hwc_request(&f.b, req, sizeof(req), resp, sizeof(resp),
				   &length) == 0);
	TEST_CHECK(length == 64 && mana_get32(resp + 12) == 64);
	TEST_CHECK(mana_get32(resp + 36) == 7 && resp[64] == 0xa5);
	r.length = 72;
	r.status = 0x1234;
	TEST_CHECK(mana_hwc_request(&f.b, req, sizeof(req), resp, sizeof(resp),
				   &length) == -EPROTO);
	TEST_CHECK(f.errors && !f.b.pending && !f.mismatch);
	r.status = 0;
	f.respond = NULL;
	TEST_CHECK(mana_hwc_request(&f.b, req, sizeof(req), resp, sizeof(resp),
				   &length) == 0);
	TEST_CHECK(mana_get32(resp + 36) == 32);
	TEST_CHECK(r.calls == 6 && r.handled == 3);
	f.respond = limits_response;
	f.fault = HOST_BOOTSTRAP_BAD_MESSAGE_ID;
	TEST_CHECK(mana_hwc_request(&f.b, req, sizeof(req), resp, sizeof(resp),
				   &length) == -EPROTO);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.maps && !f.irq_live && !f.mismatch);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

int main(void)
{
	static const struct host_test tests[] = {
		{"PCI and HWC bootstrap, request, owner wrap, cleanup", success},
		{"PCI and DMA resource rejection", invalid_resources},
		{"Each DMA allocation failure", allocations},
		{"HWC malformed events, responses, and timeouts", protocol_errors},
		{"DMA quarantine and teardown retry", quarantine},
		{"Each PCI configuration failure", config_failures},
		{"Reusable response hook, fallback, length, and faults", response_extension},
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
