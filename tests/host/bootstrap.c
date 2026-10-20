/* SPDX-License-Identifier: BSD-3-Clause */
#include "bootstrap.h"
#include "../../src/internal.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t host_bootstrap_config(const struct host_bootstrap *f, unsigned int off,
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
	struct host_bootstrap *f = ctx;
	if (++f->cfg_calls == f->cfg_fail_at)
		return -EIO;
	*v = host_bootstrap_config(f, off, width);
	if (width == 4 && off >= 0x10 && off < 0x28 && *v == UINT32_MAX)
		*v = off == 0x10 ? UINT32_C(0xffffe000) :
			(off == 0x14 && f->wide_bar ? UINT32_MAX : 0);
	return 0;
}

static int config_write(void *ctx, uint16_t off, unsigned int width, uint32_t v)
{
	struct host_bootstrap *f = ctx;
	unsigned int i;
	if (++f->cfg_calls == f->cfg_fail_at)
		return -EIO;
	for (i = 0; i < width; i++)
		f->config[off + i] = (unsigned char)(v >> (i * 8));
	return 0;
}

static int map(void *ctx, uint64_t pa, uint64_t size, struct mana_mapping *m)
{
	struct host_bootstrap *f = ctx;
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
	struct host_bootstrap *f = ctx;
	if (f->unmap_fail)
		return -EIO;
	f->maps--;
	memset(m, 0, sizeof(*m));
	return 0;
}

static uint32_t read32(void *ctx, const struct mana_mapping *m, uint64_t off)
{
	struct host_bootstrap *f = ctx;
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

void host_bootstrap_event(struct host_bootstrap *f, uint32_t type, uint32_t data)
{
	unsigned char *eq = f->b.pages[MANA_HWC_EQ].addr;
	unsigned char *entry = eq + (f->eq_producer % 256) * 16;
	mana_put32(entry, data);
	mana_put32(entry + 12, type | ((f->eq_producer / 256) & 7) << 29);
	f->eq_producer++;
}

static void completion(struct host_bootstrap *f, int sq, uint32_t offset, uint32_t len)
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

static void establish(struct host_bootstrap *f)
{
	static const uint32_t init[] = { 5, 6, 7, 1, 4096, 4096, 32, 3, 8,
			HOST_BOOTSTRAP_INIT_PF_DEST_RQ_ID,
			HOST_BOOTSTRAP_INIT_PF_DEST_CQ_ID };
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
	if (f->fault == HOST_BOOTSTRAP_NO_INIT)
		return;
	host_bootstrap_event(f, 129, f->fault == HOST_BOOTSTRAP_BAD_DB ? 0xffff0009U : 9);
	for (i = 0; i < 11; i++) {
		uint32_t v = init[i];
		if (f->fault == HOST_BOOTSTRAP_MISSING_INIT && i == 2)
			continue;
		if (f->fault == HOST_BOOTSTRAP_SMALL_REQUEST && i == 4)
			v = 639;
		if (f->fault == HOST_BOOTSTRAP_BAD_CQ && i == 0)
			v = 32;
		host_bootstrap_event(f, 130, ((i + 1) << 24) | v);
		if (f->fault == HOST_BOOTSTRAP_DUP_INIT && i == 0)
			host_bootstrap_event(f, 130, (1U << 24) | v);
	}
	host_bootstrap_event(f, 131, 0);
	if (f->fault == HOST_BOOTSTRAP_UNKNOWN_EVENT)
		host_bootstrap_event(f, 255, 0);
	if (f->fault == HOST_BOOTSTRAP_BAD_OWNER)
		mana_put32((unsigned char *)f->b.pages[0].addr + 12,
			   129 | (3U << 29));
	if (f->fault != HOST_BOOTSTRAP_NO_IRQ)
		f->handler(f->arg);
}

static void write32(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint32_t v)
{
	struct host_bootstrap *f = ctx;
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
			mana_put32(f->host.bar + off, f->fault == HOST_BOOTSTRAP_SMC_STATUS ? 0x181 :
				   (f->fault == HOST_BOOTSTRAP_BAD_SMC_TYPE ? 0x82 : 0x81));
			establish(f);
		} else if (v == 2) {
			f->smc_destroy++;
			mana_put32(f->host.bar + off, f->fault == HOST_BOOTSTRAP_DESTROY_TIMEOUT ?
				   UINT32_C(0x80000002) : 0x82);
		}
	}
}

/*
 * Complete the control request that the driver last doorbelled. The synchronous
 * model calls it inside the doorbell. The asynchronous device model schedules
 * it at a later device time. See tests/host/device.h.
 */
void host_bootstrap_request(struct host_bootstrap *f)
{
	const unsigned char *tx = f->b.pages[MANA_HWC_TX].addr;
	unsigned char *rx = f->b.pages[MANA_HWC_RX].addr;
	unsigned char *sq = f->b.pages[MANA_HWC_SQ].addr;
	unsigned char *rq = f->b.pages[MANA_HWC_RQ].addr;
	unsigned int sqoff = ((f->b.sq_head - 2) * 32) & 4095;
	unsigned int rqoff = f->b.rx_offset;
	uint32_t type = mana_get32(tx + 4), size = mana_get32(tx + 28);
	uint32_t device_type = mana_get32(tx + 32);
	uint32_t device_instance = mana_get32(tx + 36);
	size_t length = size;
	int err = -ENOTSUP;

	f->requests++;
	if (mana_get32(sq + sqoff + 4) != (1 | (6U << 8)) ||
	    mana_get32(sq + sqoff + 16) != HOST_BOOTSTRAP_INIT_PF_DEST_RQ_ID ||
	    mana_get32(sq + sqoff + 20) != HOST_BOOTSTRAP_INIT_PF_DEST_CQ_ID ||
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
	memcpy(rx, tx + 16, 16);
	mana_put32(rx + 16, mana_get32(tx + 32));
	mana_put32(rx + 20, mana_get32(tx + 36));
	if (f->respond)
		err = f->respond(f->respond_ctx, tx, mana_get32(tx + 12),
				 rx, 4096, &length);
	if (err == -ENOTSUP && type == 10 && device_type == 1 &&
	    device_instance == 0) {
		if (mana_get32(tx + 40) != 9)
			f->mismatch = 1;
		host_bootstrap_event(f, 64, 0);
		err = 0;
	} else if (err == -ENOTSUP && type == 1 && device_type == 0 &&
		   device_instance == 0) {
		if (mana_get32(tx + 12) != 640 || mana_get64(tx + 40) != 1 ||
		    mana_get64(tx + 48) != 1 || mana_get64(tx + 56) != 0 ||
		    mana_get32(tx + 96) != 0x30)
			f->mismatch = 1;
		mana_put64(rx + 32, f->fault == HOST_BOOTSTRAP_BAD_VERSION ? 2 : 1);
		err = 0;
	} else if (err == -ENOTSUP && type == 3 && device_type == 0 &&
		   device_instance == 0) {
		mana_put32(rx + 32, f->fault == HOST_BOOTSTRAP_NO_MANA ? 1 : 2);
		mana_put32(rx + 40, 1);
		if (f->fault != HOST_BOOTSTRAP_NO_MANA)
			mana_put32(rx + 48, 0x10002); /* A hole before MANA. */
		err = 0;
	} else if (err == -ENOTSUP && type == 2 && device_type == 0 &&
		   device_instance == 0) {
		unsigned int i;
		for (i = 1; i < 10; i++)
			mana_put32(rx + 32 + i * 4, 32);
		err = 0;
	}
	if (err || length < 32 || length > 4096) {
		fprintf(stderr, "mock response failed: command %u, device %u:%u, "
			"error %d, length %zu\n", type, device_type,
			device_instance, err ? err : -EPROTO, length);
		abort();
	}
	mana_put32(rx + 12, (uint32_t)length);
	if (f->fault == HOST_BOOTSTRAP_BAD_MESSAGE_ID)
		rx[10] = 1;
	if (f->fault == HOST_BOOTSTRAP_BAD_STATUS)
		mana_put32(rx + 24, 0x1234);
	completion(f, 0, rqoff, f->fault == HOST_BOOTSTRAP_BAD_LENGTH ? 4097 :
		   (f->fault == HOST_BOOTSTRAP_SHORT_RESPONSE ? 31 : (uint32_t)length));
	if (f->fault != HOST_BOOTSTRAP_NO_TX)
		completion(f, 1, sqoff, 0);
	host_bootstrap_event(f, 3, f->fault == HOST_BOOTSTRAP_WRONG_QUEUE ? 32 : 5);
	if (f->fault != HOST_BOOTSTRAP_NO_IRQ)
		f->handler(f->arg);
}

static void write64(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint64_t v)
{
	struct host_bootstrap *f = ctx;
	int err;
	if (m->addr != f->host.bar)
		abort();
	err = host_mock_write(&f->host, (size_t)off, 8, v);
	if (err) {
		fprintf(stderr, "mock doorbell write failed: %d\n", err);
		abort();
	}
	if (off == 0x1000) {
		if ((v & 0xffffff) != 7 || (uint32_t)(v >> 32) != f->b.sq_head * 32 ||
		    v >> 63)
			f->mismatch = 1;
		host_bootstrap_request(f);
	} else if (off == 0x1400 && (uint32_t)v == f->b.rq_id) {
		f->rq_bells++;
		if ((uint32_t)(v >> 32) != f->b.rq_head * 32 || v >> 63)
			f->mismatch = 1;
	} else if (off == 0x1800 && (uint32_t)v == f->b.cq_id) {
		f->cq_bells++;
		if ((uint32_t)(v >> 32) != (f->b.cq_head % 512 | 0x80000000U))
			f->mismatch = 1;
	} else if (off == 0x1ff8 && (uint32_t)v == f->b.eq_id) {
		f->eq_bells++;
		if ((uint32_t)(v >> 32) != (f->b.eq_head % 2048 | 0x80000000U))
			f->mismatch = 1;
	}
}

static int dma_alloc(void *ctx, size_t size, struct mana_dma *m)
{
	struct host_bootstrap *f = ctx;
	struct host_dma d;
	size_t i;
	int err = host_mock_alloc(&f->host, size, 4096, &d);
	f->dma_calls++;
	if (!err) {
		*m = (struct mana_dma){d.address, d.device_address, d.size};
		if (f->dma_calls == f->dma_bad_at)
			m->device_addr++;
		if (f->dma_high_at && f->dma_calls == f->dma_high_at) {
			m->device_addr = f->dma_high_value;
			for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++)
				if (f->host.dma[i].address == d.address) {
					f->host.dma[i].device_address =
						f->dma_high_value;
					break;
				}
		}
	}
	return err;
}

static void dma_free(void *ctx, struct mana_dma *m)
{
	struct host_bootstrap *f = ctx;
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
	struct host_bootstrap *f = ctx;
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
	struct host_bootstrap *f = ctx;
	(void)irq;
	if (f->irq_free_fail)
		return -EIO;
	f->irq_live = 0;
	return 0;
}

static int delay_us(void *ctx, unsigned int us)
{
	struct host_bootstrap *f = ctx;
	if (f->clock_fail)
		return -EIO;
	return host_mock_advance(&f->host, us);
}

static void error(void *ctx, const char *stage, int err)
{
	struct host_bootstrap *f = ctx;
	(void)stage;
	if (err >= 0)
		abort();
	f->errors++;
}

void host_bootstrap_init(struct host_bootstrap *f, enum host_bootstrap_fault fault)
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
		   fault == HOST_BOOTSTRAP_SMC_BUSY ? UINT32_C(0x80000000) : 0);
	memset(f->host.bar + 0x400, 0x5a, 16);
	memcpy(f->table_saved, f->host.bar + 0x400, 16);
}
