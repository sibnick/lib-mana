/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include "../src/pci_ids.h"

static int success(void)
{
	struct host_bootstrap f;
	unsigned char req[40] = { 0 }, resp[72];
	size_t length;
	unsigned int i;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	/* Allocate so the last ring page lands 4 KiB below the 2^52 device
	 * address limit. The model verifies the SMC packing there.
	 */
	f.host.next_device_address = UINT64_C(0x10000000000000) - 6 * 4096;
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

/* Establish faults that fail before INIT_DONE must not send an SMC destroy.
 * Faults after a validated establish still get exactly one destroy.
 */
static const struct {
	enum host_bootstrap_fault fault;
	unsigned int destroy;
} start_faults[] = {
	{ HOST_BOOTSTRAP_SMC_STATUS, 0 },
	{ HOST_BOOTSTRAP_NO_INIT, 0 },
	{ HOST_BOOTSTRAP_BAD_OWNER, 0 },
	{ HOST_BOOTSTRAP_DUP_INIT, 0 },
	{ HOST_BOOTSTRAP_SMALL_REQUEST, 0 },
	{ HOST_BOOTSTRAP_BAD_DB, 0 },
	{ HOST_BOOTSTRAP_BAD_CQ, 0 },
	{ HOST_BOOTSTRAP_MISSING_INIT, 0 },
	{ HOST_BOOTSTRAP_BAD_LENGTH, 1 },
	{ HOST_BOOTSTRAP_BAD_MESSAGE_ID, 1 },
	{ HOST_BOOTSTRAP_BAD_STATUS, 1 },
	{ HOST_BOOTSTRAP_NO_TX, 1 },
	{ HOST_BOOTSTRAP_NO_IRQ, 1 },
	{ HOST_BOOTSTRAP_NO_MANA, 1 },
	{ HOST_BOOTSTRAP_BAD_VERSION, 1 },
	{ HOST_BOOTSTRAP_WRONG_QUEUE, 1 },
	{ HOST_BOOTSTRAP_SHORT_RESPONSE, 1 },
	{ HOST_BOOTSTRAP_BAD_SMC_TYPE, 0 },
};

static int device_address_bound(void)
{
	struct host_bootstrap f;
	unsigned int i;
	/* A page at exactly 2^52 must fail before any SMC or bus-master use. */
	for (i = 1; i <= 6; i++) {
		host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
		f.dma_high_at = i;
		f.dma_high_value = UINT64_C(0x10000000000000);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EPROTO);
		TEST_CHECK(!f.smc_destroy && !f.mismatch && !f.b.dma_exposed);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(!f.b.quarantined);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	/* The highest legal pages, ending 4 KiB below 2^52, are accepted.
	 * An unaligned 2^52-1 address is rejected by the alignment gate.
	 */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	f.host.next_device_address = UINT64_C(0x10000000000000) - 6 * 4096;
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.pages[MANA_HWC_TX].device_addr ==
		   UINT64_C(0x10000000000000) - 4096);
	TEST_CHECK(!f.mismatch);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(f.host.live_allocations == 0 && f.smc_destroy == 1);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int protocol_errors(void)
{
	struct host_bootstrap f;
	unsigned int i;
	for (i = 0; i < sizeof(start_faults) / sizeof(start_faults[0]); i++) {
		host_bootstrap_init(&f, start_faults[i].fault);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(f.smc_destroy == start_faults[i].destroy);
		TEST_CHECK(!f.b.quarantined && !f.b.ready);
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

/* Events the pinned dispatcher ignores must not block readiness. The pin
 * drops undefined types (gdma_main.c:973-974) and service events without
 * a local consumer (hw_channel.c:215-236). This driver counts them and
 * keeps the established channel usable.
 */
static int ignored_establish_events(void)
{
	struct host_bootstrap f;
	host_bootstrap_init(&f, HOST_BOOTSTRAP_UNKNOWN_EVENT);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.ready && f.b.initialized);
	TEST_CHECK(f.b.ignored_events == 3);
	TEST_CHECK(!f.b.error && !f.b.quarantined);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.b.quarantined && !f.host.live_allocations && !f.maps && !f.irq_live);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int establish_gate(void)
{
	struct host_bootstrap f;
	/* The PF never confirms the establish. Stop must not send a destroy. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NO_INIT);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -ETIMEDOUT);
	TEST_CHECK(f.smc_destroy == 0 && !f.b.initialized);
	TEST_CHECK(!f.b.dma_exposed);
	TEST_CHECK(f.errors && !f.host.live_allocations && !f.maps && !f.irq_live);
	TEST_CHECK(!f.b.quarantined);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	/* An invalid establish response also skips the destroy. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_SMC_STATUS);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EPROTO);
	TEST_CHECK(f.smc_destroy == 0 && !f.b.initialized);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	/* A validated establish still gets exactly one confirmed destroy. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(f.smc_destroy == 1 && !f.b.quarantined);
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

static int msix_readback(void)
{
	struct host_bootstrap f;
	/* A silently dropped MSI-X control write must abort the start. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	f.msix_control_stuck = 1;
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EIO);
	TEST_CHECK(f.errors && !f.b.ready && !f.b.quarantined);
	TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
	TEST_CHECK(host_bootstrap_config(&f, 0x42, 2) == 0);
	TEST_CHECK(host_bootstrap_config(&f, 4, 2) == 0);
	TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	/* A readback mismatch during stop quarantines. A retry completes. */
	host_bootstrap_init(&f, HOST_BOOTSTRAP_NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	f.msix_control_stuck = 1;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == -EIO);
	TEST_CHECK(f.b.quarantined && f.host.live_allocations == 6 && f.irq_live);
	TEST_CHECK(host_bootstrap_config(&f, 0x42, 2) == 0x8000);
	f.msix_control_stuck = 0;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.b.quarantined && !f.host.live_allocations && !f.irq_live);
	TEST_CHECK(host_bootstrap_config(&f, 0x42, 2) == 0);
	TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
	TEST_CHECK(f.smc_destroy == 1 && !f.maps);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
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

/*
 * Probe matching follows the pinned core: drivers/ukbus/pci/pci_bus.c in
 * Unikraft e31b2c44effe583eb8910e713fda814b6e049213. pci_find_driver stops
 * at the first all-ANY entry, so that entry terminates the list.
 */
static int probe_id_is_any(const struct pci_device_id *id)
{
	return id->class_id == PCI_CLASS_ANY_ID &&
		id->sub_class_id == PCI_CLASS_ANY_ID &&
		id->vendor_id == PCI_ANY_ID && id->device_id == PCI_ANY_ID &&
		id->subsystem_vendor_id == PCI_ANY_ID &&
		id->subsystem_device_id == PCI_ANY_ID;
}

static int probe_id_match(const struct pci_device_id *dev,
			  const struct pci_device_id *drv)
{
	if (dev->class_id != PCI_CLASS_ANY_ID &&
	    drv->class_id != PCI_CLASS_ANY_ID &&
	    dev->class_id != drv->class_id)
		return 0;
	if (dev->sub_class_id != PCI_CLASS_ANY_ID &&
	    drv->sub_class_id != PCI_CLASS_ANY_ID &&
	    dev->sub_class_id != drv->sub_class_id)
		return 0;
	if (dev->vendor_id != PCI_ANY_ID && drv->vendor_id != PCI_ANY_ID &&
	    dev->vendor_id != drv->vendor_id)
		return 0;
	if (dev->device_id != PCI_ANY_ID && drv->device_id != PCI_ANY_ID &&
	    dev->device_id != drv->device_id)
		return 0;
	if (dev->subsystem_vendor_id != PCI_ANY_ID &&
	    drv->subsystem_vendor_id != PCI_ANY_ID &&
	    dev->subsystem_vendor_id != drv->subsystem_vendor_id)
		return 0;
	if (dev->subsystem_device_id != PCI_ANY_ID &&
	    drv->subsystem_device_id != PCI_ANY_ID &&
	    dev->subsystem_device_id != drv->subsystem_device_id)
		return 0;
	return 1;
}

static int probe_table_matches(const struct pci_device_id *dev)
{
	const struct pci_device_id *drv_id;

	for (drv_id = mana_pci_ids; !probe_id_is_any(drv_id); drv_id++)
		if (probe_id_match(dev, drv_id))
			return 1;
	return 0;
}

static int pci_id_table(void)
{
	struct pci_device_id dev;
	unsigned int i, count = 0;

	/* The table holds exactly the ids include/uk/mana.h declares. */
	for (i = 0; !probe_id_is_any(&mana_pci_ids[i]); i++)
		count++;
	TEST_CHECK(count == 1);
	TEST_CHECK(mana_pci_ids[0].vendor_id == MANA_PCI_VENDOR_ID);
	TEST_CHECK(mana_pci_ids[0].device_id == MANA_PCI_VF_DEVICE_ID);
	/* The MANA VF matches with any class and subsystem values. */
	dev = (struct pci_device_id){ 0x02000002, 0x00, 0x1414, 0x00ba,
				      0x1414, 0x00ba };
	TEST_CHECK(probe_table_matches(&dev));
	dev = (struct pci_device_id){ 0x00000000, 0x00, 0x1414, 0x00ba,
				      0x0000, 0x0000 };
	TEST_CHECK(probe_table_matches(&dev));
	/* Non-MANA devices never reach mana_add. */
	dev = (struct pci_device_id){ 0x02000000, 0x00, 0x1414, 0x00b1,
				      0x1414, 0x00b1 }; /* MANA PF. */
	TEST_CHECK(!probe_table_matches(&dev));
	dev = (struct pci_device_id){ 0x02000000, 0x00, 0x1413, 0x00ba,
				      0x0001, 0x0002 };
	TEST_CHECK(!probe_table_matches(&dev));
	dev = (struct pci_device_id){ 0x02000001, 0x00, 0x8086, 0x100e,
				      0x8086, 0x0001 };
	TEST_CHECK(!probe_table_matches(&dev));
	dev = (struct pci_device_id){ 0x01000000, 0x00, 0x1af4, 0x1041,
				      0x1af4, 0x0001 };
	TEST_CHECK(!probe_table_matches(&dev));
	dev = (struct pci_device_id){ 0x06040000, 0x04, 0x10ec, 0x8136,
				      0x1af4, 0x1105 };
	TEST_CHECK(!probe_table_matches(&dev));
	return 0;
}

int main(void)
{
	static const struct host_test tests[] = {
		{"PCI id table scope against the pinned core match", pci_id_table},
		{"PCI and HWC bootstrap, request, owner wrap, cleanup", success},
		{"PCI and DMA resource rejection", invalid_resources},
		{"Each DMA allocation failure", allocations},
		{"Device address 2^52 boundary before SMC packing", device_address_bound},
		{"HWC malformed events, responses, and timeouts", protocol_errors},
		{"Events the pin ignores do not block establish", ignored_establish_events},
		{"SMC destroy gated by a validated establish", establish_gate},
		{"DMA quarantine and teardown retry", quarantine},
		{"Each PCI configuration failure", config_failures},
		{"Silent MSI-X control write failure aborts and quarantines", msix_readback},
		{"Reusable response hook, fallback, length, and faults", response_extension},
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
