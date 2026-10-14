/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/mock.h"
#include "../src/internal.h"
#include <stdlib.h>

enum fault {
	NORMAL, SMC_BUSY, SMC_STATUS, NO_INIT, BAD_OWNER, DUP_INIT,
	SMALL_REQUEST, BAD_DB, BAD_CQ, BAD_LENGTH, BAD_MESSAGE_ID,
	BAD_STATUS, NO_TX, NO_IRQ, NO_MANA, BAD_VERSION, DESTROY_TIMEOUT,
	UNKNOWN_EVENT, MISSING_INIT, WRONG_QUEUE, SHORT_RESPONSE, BAD_SMC_TYPE,
};

struct fixture {
	struct host_mock host;
	struct mana_platform platform;
	struct mana_bootstrap b;
	unsigned char config[256];
	unsigned char table_saved[16];
	enum fault fault;
	void (*handler)(void *);
	void *arg;
	unsigned int irq_live, maps, errors, smc_destroy, requests;
	unsigned int cfg_calls, cfg_fail_at, map_fail, irq_fail, unmap_fail;
	unsigned int irq_free_fail, dma_bad_at, dma_calls, wide_bar, clock_fail;
	uint32_t eq_producer, cq_producer;
	int mismatch;
};

static uint32_t get_config(struct fixture *f, unsigned int off,
			   unsigned int width)
{
	uint32_t v = 0;
	unsigned int i;
	for (i = 0; i < width; i++)
		v |= (uint32_t)f->config[off + i] << (i * 8);
	return v;
}

static int config_read(void *ctx, uint16_t off, unsigned int width, uint32_t *v)
{
	struct fixture *f = ctx;
	if (++f->cfg_calls == f->cfg_fail_at)
		return -EIO;
	*v = get_config(f, off, width);
	if (width == 4 && off >= 0x10 && off < 0x28 && *v == UINT32_MAX)
		*v = off == 0x10 ? UINT32_C(0xffffe000) :
			(off == 0x14 && f->wide_bar ? UINT32_MAX : 0);
	return 0;
}

static int config_write(void *ctx, uint16_t off, unsigned int width, uint32_t v)
{
	struct fixture *f = ctx;
	unsigned int i;
	if (++f->cfg_calls == f->cfg_fail_at)
		return -EIO;
	for (i = 0; i < width; i++)
		f->config[off + i] = (unsigned char)(v >> (i * 8));
	return 0;
}

static int map(void *ctx, uint64_t pa, uint64_t size, struct mana_mapping *m)
{
	struct fixture *f = ctx;
	if (f->map_fail)
		return -ENOMEM;
	if (pa != (f->wide_bar ? UINT64_C(0x100000000) : UINT64_C(0x80000000)) ||
	    size != sizeof(f->host.bar))
		return -EINVAL;
	m->addr = f->host.bar;
	m->size = size;
	f->maps++;
	return 0;
}

static int unmap(void *ctx, struct mana_mapping *m)
{
	struct fixture *f = ctx;
	if (f->unmap_fail)
		return -EIO;
	f->maps--;
	memset(m, 0, sizeof(*m));
	return 0;
}

static uint32_t read32(void *ctx, const struct mana_mapping *m, uint64_t off)
{
	struct fixture *f = ctx;
	uint64_t v;
	int err;
	if (m->addr != f->host.bar)
		abort();
	err = host_mock_read(&f->host, (size_t)off, 4, &v);
	if (err) {
		fprintf(stderr, "mock MMIO read failed: %d\n", err);
		abort();
	}
	return (uint32_t)v;
}

static void event(struct fixture *f, uint32_t type, uint32_t data)
{
	unsigned char *eq = f->b.pages[MANA_HWC_EQ].addr;
	unsigned char *entry = eq + (f->eq_producer % 256) * 16;
	mana_put32(entry, data);
	mana_put32(entry + 12, type | ((f->eq_producer / 256) & 7) << 29);
	f->eq_producer++;
}

static void completion(struct fixture *f, int sq, uint32_t offset, uint32_t len)
{
	unsigned char *cq = f->b.pages[MANA_HWC_CQ].addr;
	unsigned char *entry = cq + (f->cq_producer % 64) * 64;
	memset(entry, 0, 64);
	mana_put32(entry + 12, offset);
	mana_put32(entry + 24, len);
	mana_put32(entry + 60, (sq ? 7U | (1U << 24) : 6U) |
		   ((f->cq_producer / 64) & 7) << 29);
	f->cq_producer++;
}

static void establish(struct fixture *f)
{
	static const uint32_t init[] = { 5, 6, 7, 1, 4096, 4096, 32, 3, 8, 0, 0 };
	unsigned int i, j;
	uint16_t high = f->host.bar[0x118] | (uint16_t)f->host.bar[0x119] << 8;
	for (i = 0; i < 4; i++) {
		uint64_t frame = (uint64_t)((high >> (i * 4)) & 15) << 48;
		for (j = 0; j < 6; j++)
			frame |= (uint64_t)f->host.bar[0x100 + i * 6 + j] << (j * 8);
		if ((frame << 12) != f->b.pages[i].device_addr)
			f->mismatch = 1;
	}
	if (f->host.bar[0x11a] || f->host.bar[0x11b])
		f->mismatch = 1;
	if (f->fault == NO_INIT)
		return;
	event(f, 129, f->fault == BAD_DB ? 0xffff0009U : 9);
	for (i = 0; i < 11; i++) {
		uint32_t v = init[i];
		if (f->fault == MISSING_INIT && i == 2)
			continue;
		if (f->fault == SMALL_REQUEST && i == 4)
			v = 639;
		if (f->fault == BAD_CQ && i == 0)
			v = 32;
		event(f, 130, ((i + 1) << 24) | v);
		if (f->fault == DUP_INIT && i == 0)
			event(f, 130, (1U << 24) | v);
	}
	event(f, 131, 0);
	if (f->fault == UNKNOWN_EVENT)
		event(f, 255, 0);
	if (f->fault == BAD_OWNER)
		mana_put32((unsigned char *)f->b.pages[0].addr + 12,
			   129 | (3U << 29));
	if (f->fault != NO_IRQ)
		f->handler(f->arg);
}

static void write32(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint32_t v)
{
	struct fixture *f = ctx;
	int err;
	if (m->addr != f->host.bar)
		abort();
	err = host_mock_write(&f->host, (size_t)off, 4, v);
	if (err) {
		fprintf(stderr, "mock MMIO write failed: %d\n", err);
		abort();
	}
	if (off == 0x11c) {
		if (v == 1) {
			mana_put32(f->host.bar + off, f->fault == SMC_STATUS ? 0x181 :
				   (f->fault == BAD_SMC_TYPE ? 0x82 : 0x81));
			establish(f);
		} else if (v == 2) {
			f->smc_destroy++;
			mana_put32(f->host.bar + off, f->fault == DESTROY_TIMEOUT ?
				   UINT32_C(0x80000002) : 0x82);
		}
	}
}

static void request(struct fixture *f)
{
	const unsigned char *tx = f->b.pages[MANA_HWC_TX].addr;
	unsigned char *rx = f->b.pages[MANA_HWC_RX].addr;
	unsigned char *sq = f->b.pages[MANA_HWC_SQ].addr;
	unsigned char *rq = f->b.pages[MANA_HWC_RQ].addr;
	unsigned int sqoff = ((f->b.sq_head - 2) * 32) & 4095;
	unsigned int rqoff = f->b.rx_offset;
	uint32_t type = mana_get32(tx + 4), size = mana_get32(tx + 28);

	f->requests++;
	if (mana_get32(sq + sqoff + 4) != (1 | (6U << 8)) ||
	    mana_get64(sq + sqoff + 32) != f->b.pages[MANA_HWC_TX].device_addr ||
	    mana_get32(sq + sqoff + 40) != 8 ||
	    mana_get32(sq + sqoff + 44) != mana_get32(tx + 12) ||
	    mana_get32(sq + sqoff + 24) != 5 || mana_get32(sq + sqoff + 28) != 7 ||
	    mana_get32(rq + rqoff + 4) != (1 | (2U << 8)) ||
	    mana_get64(rq + rqoff + 16) != f->b.pages[MANA_HWC_RX].device_addr ||
	    mana_get32(rq + rqoff + 24) != 8 ||
	    mana_get32(rq + rqoff + 28) != 4096)
		f->mismatch = 1;
	memset(rx, 0, 4096);
	memcpy(rx, tx, 16);
	mana_put32(rx + 12, size);
	mana_put32(rx + 16, mana_get32(tx + 32));
	mana_put32(rx + 20, mana_get32(tx + 36));
	if (f->fault == BAD_MESSAGE_ID)
		rx[10] = 1;
	if (f->fault == BAD_STATUS)
		mana_put32(rx + 24, 0x1234);
	if (type == 10) {
		if (mana_get32(tx + 40) != 9 || mana_get32(tx + 32) != 1)
			f->mismatch = 1;
		event(f, 64, 0);
	} else if (type == 1) {
		if (mana_get32(tx + 12) != 640 || mana_get64(tx + 40) != 1 ||
		    mana_get64(tx + 48) != 1 || mana_get64(tx + 56) != 0 ||
		    mana_get32(tx + 96) != 0x30)
			f->mismatch = 1;
		mana_put64(rx + 32, f->fault == BAD_VERSION ? 2 : 1);
	} else if (type == 3) {
		mana_put32(rx + 32, f->fault == NO_MANA ? 1 : 2);
		mana_put32(rx + 40, 1);
		if (f->fault != NO_MANA)
			mana_put32(rx + 48, 0x10002); /* A hole before MANA. */
	} else if (type == 2) {
		unsigned int i;
		for (i = 1; i < 10; i++)
			mana_put32(rx + 32 + i * 4, 32);
	}
	completion(f, 0, rqoff, f->fault == BAD_LENGTH ? 4097 :
		   (f->fault == SHORT_RESPONSE ? 31 : size));
	if (f->fault != NO_TX)
		completion(f, 1, sqoff, 0);
	event(f, 3, f->fault == WRONG_QUEUE ? 32 : 5);
	if (f->fault != NO_IRQ)
		f->handler(f->arg);
}

static void write64(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint64_t v)
{
	struct fixture *f = ctx;
	int err;
	if (m->addr != f->host.bar)
		abort();
	err = host_mock_write(&f->host, (size_t)off, 8, v);
	if (err) {
		fprintf(stderr, "mock doorbell write failed: %d\n", err);
		abort();
	}
	if (off == 0x1000) {
		if ((v & 0xffffff) != 7 || (uint32_t)(v >> 32) != f->b.sq_head * 32)
			f->mismatch = 1;
		request(f);
	}
}

static int dma_alloc(void *ctx, size_t size, struct mana_dma *m)
{
	struct fixture *f = ctx;
	struct host_dma d;
	int err = host_mock_alloc(&f->host, size, 4096, &d);
	f->dma_calls++;
	if (!err) {
		*m = (struct mana_dma){d.address, d.device_address, d.size};
		if (f->dma_calls == f->dma_bad_at)
			m->device_addr++;
	}
	return err;
}

static void dma_free(void *ctx, struct mana_dma *m)
{
	struct fixture *f = ctx;
	struct host_dma d = {m->addr, m->size, m->device_addr & ~UINT64_C(4095)};
	if (host_mock_free(&f->host, &d))
		abort();
	memset(m, 0, sizeof(*m));
}

static void barrier(void *ctx)
{
	(void)ctx;
}

static int irq_alloc(void *ctx, void (*handler)(void *), void *arg,
		     struct mana_irq *irq)
{
	struct fixture *f = ctx;
	if (f->irq_fail)
		return -ENOSPC;
	f->handler = handler;
	f->arg = arg;
	f->irq_live = 1;
	*irq = (struct mana_irq){20, UINT64_C(0xfee01000), 52};
	return 0;
}

static int irq_free(void *ctx, struct mana_irq *irq)
{
	struct fixture *f = ctx;
	(void)irq;
	if (f->irq_free_fail)
		return -EIO;
	f->irq_live = 0;
	return 0;
}

static int delay_us(void *ctx, unsigned int us)
{
	struct fixture *f = ctx;
	if (f->clock_fail)
		return -EIO;
	return host_mock_advance(&f->host, us);
}

static void error(void *ctx, const char *stage, int err)
{
	struct fixture *f = ctx;
	(void)stage;
	if (err >= 0)
		abort();
	f->errors++;
}

static void init(struct fixture *f, enum fault fault)
{
	memset(f, 0, sizeof(*f));
	host_mock_init(&f->host);
	f->fault = fault;
	f->eq_producer = 256;
	f->cq_producer = 64;
	f->platform = (struct mana_platform) {
		.ctx = f, .dma_guarantees = MANA_DMA_REQUIRED,
		.config_read = config_read, .config_write = config_write,
		.map = map, .unmap = unmap,
		.read32 = read32, .write32 = write32, .write64 = write64,
		.dma_alloc = dma_alloc, .dma_free = dma_free,
		.dma_rmb = barrier, .dma_wmb = barrier,
		.irq_alloc = irq_alloc, .irq_free = irq_free,
		.delay_us = delay_us, .error = error,
	};
	mana_put32(f->config, UINT32_C(0x00ba1414));
	mana_put32(f->config + 4, 0x100000);
	mana_put32(f->config + 0x10, UINT32_C(0x80000000));
	f->config[0x34] = 0x40;
	mana_put32(f->config + 0x40, 0x11);
	mana_put32(f->config + 0x44, 0x400);
	mana_put32(f->config + 0x48, 0x440);
	mana_put64(f->host.bar + 8, 4096);
	mana_put32(f->host.bar + 0x10, 4096);
	mana_put64(f->host.bar + 0x18, 0x100);
	mana_put32(f->host.bar + 0x11c,
		   fault == SMC_BUSY ? UINT32_C(0x80000000) : 0);
	memset(f->host.bar + 0x400, 0x5a, 16);
	memcpy(f->table_saved, f->host.bar + 0x400, 16);
}

static int success(void)
{
	struct fixture f;
	unsigned char req[40] = { 0 }, resp[72];
	size_t length;
	unsigned int i;
	init(&f, NORMAL);
	/* Also exercise high page-frame nibbles in the SMC message. */
	f.host.next_device_address = UINT64_C(0x1234567890000000);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.ready && f.b.mana_device_id == 0x10002);
	TEST_CHECK(f.requests == 4 && !f.mismatch && f.b.limits.msix == 32);
	TEST_CHECK(f.host.live_allocations == 6 && f.irq_live);
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
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.b.ready && !f.irq_live && !f.maps);
	TEST_CHECK(f.host.live_allocations == 0 && f.smc_destroy == 1);
	TEST_CHECK(get_config(&f, 4, 2) == 0 && get_config(&f, 0x42, 2) == 0);
	TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	init(&f, NORMAL);
	f.wide_bar = 1;
	mana_put32(f.config + 0x10, 4);
	mana_put32(f.config + 0x14, 1);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(f.b.bars[0].physical == UINT64_C(0x100000000));
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(get_config(&f, 0x10, 4) == 4 && get_config(&f, 0x14, 4) == 1);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	init(&f, NORMAL);
	mana_put32(f.config + 0x10, UINT32_C(0x80000008));
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int invalid_resources(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 0; i < 19; i++) {
		init(&f, NORMAL);
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
	return 0;
}

static int allocations(void)
{
	struct fixture f;
	unsigned int i;
	for (i = 1; i <= 6; i++) {
		init(&f, NORMAL);
		TEST_CHECK(host_fault_arm(&f.host, HOST_FAULT_ALLOC, i, -ENOMEM) == 0);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -ENOMEM);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(!f.smc_destroy && !f.b.quarantined);
		TEST_CHECK(!memcmp(f.table_saved, f.host.bar + 0x400, 16));
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
		init(&f, NORMAL);
		f.dma_bad_at = i;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EPROTO);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	return 0;
}

static int protocol_errors(void)
{
	struct fixture f;
	enum fault faults[] = {
		SMC_STATUS, NO_INIT, BAD_OWNER, DUP_INIT, SMALL_REQUEST,
		BAD_DB, BAD_CQ, BAD_LENGTH, BAD_MESSAGE_ID, BAD_STATUS,
		NO_TX, NO_IRQ, NO_MANA, BAD_VERSION,
		UNKNOWN_EVENT, MISSING_INIT, WRONG_QUEUE, SHORT_RESPONSE, BAD_SMC_TYPE,
	};
	unsigned int i;
	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		init(&f, faults[i]);
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(f.smc_destroy == 1 && !f.b.quarantined && !f.b.ready);
		TEST_CHECK(f.host.now_us <= UINT64_C(60000000));
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
		init(&f, SMC_BUSY);
		f.clock_fail = 1;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EIO);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	init(&f, SMC_BUSY);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -ETIMEDOUT);
	TEST_CHECK(f.host.now_us == UINT64_C(20000000));
	TEST_CHECK(!f.host.live_allocations && !f.smc_destroy);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	init(&f, NORMAL);
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
	struct fixture f;
	init(&f, NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	f.fault = DESTROY_TIMEOUT;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == -ETIMEDOUT);
	TEST_CHECK(f.b.quarantined && !f.b.ready);
	TEST_CHECK(f.host.live_allocations == 6 && f.maps == 1 && f.irq_live);
	TEST_CHECK(f.host.now_us == UINT64_C(20000000));
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == -EBUSY);
	f.fault = NORMAL;
	mana_put32(f.host.bar + 0x11c, 0x82);
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live && f.smc_destroy == 1);
	TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	return 0;
}

static int config_failures(void)
{
	struct fixture f;
	unsigned int count, i;
	init(&f, NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	count = f.cfg_calls;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	for (i = 1; i <= count; i++) {
		init(&f, NORMAL);
		f.cfg_fail_at = i;
		TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) < 0);
		TEST_CHECK(f.errors && !f.b.ready);
		if (f.b.quarantined)
			TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
		TEST_CHECK(!f.host.live_allocations && !f.maps && !f.irq_live);
		TEST_CHECK(get_config(&f, 0x10, 4) == UINT32_C(0x80000000));
		TEST_CHECK(get_config(&f, 4, 2) == 0);
		TEST_CHECK(host_mock_cleanup(&f.host) == 0);
	}
	/* After a confirmed destroy, retry PCI cleanup without replaying SMC. */
	init(&f, NORMAL);
	TEST_CHECK(mana_bootstrap_start(&f.b, &f.platform) == 0);
	count = f.cfg_calls;
	TEST_CHECK(mana_bootstrap_stop(&f.b) == 0);
	count = f.cfg_calls - count;
	for (i = 1; i <= count; i++) {
		init(&f, NORMAL);
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

int main(void)
{
	static const struct host_test tests[] = {
		{"PCI and HWC bootstrap, request, owner wrap, cleanup", success},
		{"PCI and DMA resource rejection", invalid_resources},
		{"Each DMA allocation failure", allocations},
		{"HWC malformed events, responses, and timeouts", protocol_errors},
		{"DMA quarantine and teardown retry", quarantine},
		{"Each PCI configuration failure", config_failures},
	};
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
