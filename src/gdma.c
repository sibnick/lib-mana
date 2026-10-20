/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2021, Microsoft Corporation. */
/* Wire formats follow Linux MANA under its BSD-3-Clause license. */
#include "internal.h"

#define SMC_POLLS 20000U
#define HWC_POLLS 60000U
#define REQUEST_POLLS 30000U

static int smc_wait(struct mana_bootstrap *b)
{
	unsigned int i;
	for (i = 0; i < SMC_POLLS; i++) {
		uint32_t v = mana_r32(b, b->shm + 28);
		if (!(v & UINT32_C(0x80000000)))
			return 0;
		{
			int err = b->p->delay_us(b->p->ctx, 1000);
			if (err)
				return err;
		}
	}
	return -ETIMEDOUT;
}

static int smc_response(struct mana_bootstrap *b, unsigned int type)
{
	uint32_t v;
	int err = smc_wait(b);
	if (err)
		return err;
	v = mana_r32(b, b->shm + 28);
	if ((v & 0xff) != (type | 0x80) || (v & 0x7fffff00))
		return -EPROTO;
	return 0;
}

static void doorbell(struct mana_bootstrap *b, unsigned int type, uint32_t id,
		     uint32_t head, unsigned int arm)
{
	static const uint32_t offsets[] = { 0xff8, 0x800, 0x400, 0 };
	uint64_t v = id | (uint64_t)head << 32;
	if (type < 2 && arm)
		v |= UINT64_C(1) << 63;
	b->p->dma_wmb(b->p->ctx);
	b->p->write64(b->p->ctx, &b->bar0,
		      b->db + (uint64_t)b->db_size * b->db_id + offsets[type], v);
}

static uint32_t load_info(const void *addr)
{
	return *(const volatile uint32_t *)addr;
}

static int owner(struct mana_bootstrap *b, uint32_t info, uint32_t head,
		 unsigned int entries)
{
	uint32_t bits = info >> 29;
	if (bits == ((head / entries - 1) & 7))
		return 0;
	if (bits != ((head / entries) & 7))
		return -EOVERFLOW;
	b->p->dma_rmb(b->p->ctx);
	return 1;
}

static int init_event(struct mana_bootstrap *b, uint32_t type, uint32_t data)
{
	uint32_t field = data >> 24;
	uint32_t value = data & 0xffffff;
	uint32_t bit;
	if (type == 129) {
		bit = 1;
		if (b->init_seen & bit)
			return -EPROTO;
		b->eq_id = data & 0xffff;
		b->db_id = data >> 16;
		if (!mana_range(b->bar0.size, b->db,
				((uint64_t)b->db_id + 1) * b->db_size))
			return -EPROTO;
		{
			uint64_t off = b->db + (uint64_t)b->db_id * b->db_size;
			if (mana_overlap(off, b->db_size, b->shm, 32) ||
			    (!b->table_bir &&
			     mana_overlap(off, b->db_size, b->table_offset,
					  b->table_length)) ||
			    (!b->pba_bir &&
			     mana_overlap(off, b->db_size, b->pba_offset,
					  b->pba_length)))
				return -EPROTO;
		}
	} else if (type == 130) {
		if (field < 1 || field > 11)
			return -EPROTO;
		bit = 1U << field;
		if (b->init_seen & bit)
			return -EPROTO;
		switch (field) {
		case 1: b->cq_id = value; break;
		case 2: b->rq_id = value; break;
		case 3: b->sq_id = value; break;
		case 4: b->depth = value; break;
		case 5: b->max_req = value; break;
		case 6: b->max_resp = value; break;
		case 7: b->max_cqs = value; break;
		case 8: b->pd_id = value; break;
		case 9: b->mkey = value; break;
		/* Like the pinned hw_channel.c:169-181. */
		case 10: b->pf_dest_vrq_id = value; break;
		case 11: b->pf_dest_vrcq_id = value; break;
		}
	} else {
		if ((b->init_seen & 0x3ff) != 0x3ff ||
		    !b->depth || b->depth > 65535 || !b->max_req ||
		    b->max_req > 4096 || !b->max_resp || b->max_resp > 4096 ||
		    !b->max_cqs || b->cq_id >= b->max_cqs ||
		    b->max_req < 640 || b->max_resp < 296 || b->initialized)
			return -EPROTO;
		b->initialized = 1;
		return 0;
	}
	b->init_seen |= bit;
	return 0;
}

static void post_rx(struct mana_bootstrap *b)
{
	unsigned char *rq = b->pages[MANA_HWC_RQ].addr;
	unsigned int off = (b->rq_head * 32) & 4095;
	memset(rq + off, 0, 32);
	mana_put32(rq + off + 4, 1 | (2U << 8));
	mana_put64(rq + off + 16, b->pages[MANA_HWC_RX].device_addr);
	mana_put32(rq + off + 24, b->mkey);
	mana_put32(rq + off + 28, b->max_resp);
	b->rx_offset = off;
	b->rq_head++;
	doorbell(b, MANA_HWC_RQ, b->rq_id, b->rq_head * 32, 0);
}

static int complete_rx(struct mana_bootstrap *b, const unsigned char *data)
{
	const unsigned char *header;
	uint32_t length = mana_get32(data + 24);
	const unsigned char *rx = b->pages[MANA_HWC_RX].addr;

	if (!b->pending || b->rx_done || mana_get32(data + 12) != b->rx_offset ||
	    length < 32 || length > b->max_resp || length > b->response_size)
		return -EPROTO;
	memcpy(b->response, rx, length);
	header = b->response;
	if (mana_get32(header) != 0 || mana_get32(header + 4) != b->pending_type ||
	    !mana_get16(header + 8) ||
	    mana_get16(header + 8) > b->pending_response_version ||
	    header[10] || header[11] ||
	    mana_get32(header + 12) != length ||
	    mana_get32(header + 16) != b->pending_device ||
	    mana_get32(header + 20) != b->pending_activity ||
	    mana_get32(header + 28) != 0)
		return -EPROTO;
	b->response_length = length;
	b->rx_done = 1;
	return 0;
}

static int poll_cq(struct mana_bootstrap *b)
{
	unsigned int i;
	int changed = 0;
	for (i = 0; i < 64; i++) {
		const unsigned char *cq = b->pages[MANA_HWC_CQ].addr;
		const unsigned char *entry = cq + (b->cq_head % 64) * 64;
		uint32_t info = load_info(entry + 60);
		unsigned char data[32];
		int err = owner(b, info, b->cq_head, 64);
		if (err <= 0) {
			if (err)
				return err;
			break;
		}
		memcpy(data, entry, sizeof(data));
		if (mana_get32(data) & 0xff00)
			return -EPROTO;
		if (info & (1U << 24)) {
			if (!b->pending || b->tx_done ||
			    (info & 0xffffff) != b->sq_id)
				return -EPROTO;
			b->tx_done = 1;
		} else {
			if ((info & 0xffffff) != b->rq_id)
				return -EPROTO;
			err = complete_rx(b, data);
			if (err)
				return err;
		}
		b->cq_head++;
		changed = 1;
	}
	if (changed)
		doorbell(b, MANA_HWC_CQ, b->cq_id, b->cq_head % 512, 1);
	return 0;
}

int mana_bootstrap_poll(struct mana_bootstrap *b)
{
	unsigned int i;
	int changed = 0, err = 0;
	if (!b || !b->p || !b->dma_exposed || b->quarantined)
		return -EINVAL;
	if (b->error)
		return b->error;
	for (i = 0; i < 256; i++) {
		const unsigned char *eq = b->pages[MANA_HWC_EQ].addr;
		const unsigned char *entry = eq + (b->eq_head % 256) * 16;
		uint32_t info = load_info(entry + 12);
		uint32_t data;
		err = owner(b, info, b->eq_head, 256);
		if (err <= 0)
			break;
		data = mana_get32(entry);
		err = 0;
		switch (info & 0xff) {
		case 129:
		case 130:
		case 131:
			err = b->initialized ? -EPROTO :
				init_event(b, info & 0xff, data);
			break;
		case 3:
			err = !b->initialized || (data & 0xffffff) != b->cq_id ?
				-EPROTO : poll_cq(b);
			break;
		case 64:
			err = !b->initialized ? -EPROTO : 0;
			b->test_event = !err;
			break;
		case 133:
			/* A timeout update does not change this driver's bound. */
			switch (data >> 24) {
			case 1:
				break;
			case 2:
			case 3:
				b->link_known = 1;
				b->link_up = (data >> 24) == 2;
				b->link_generation++;
				break;
			default:
				err = -ENOTSUP;
				break;
			}
			break;
		case 132:
		case 134:
		case 135:
			/* No safe VF reset or service recovery exists on this platform. */
			err = -ENOTSUP;
			break;
		default:
			err = -EPROTO;
			break;
		}
		if (err)
			break;
		b->eq_head++;
		changed = 1;
	}
	if (err < 0) {
		b->error = err;
		b->p->error(b->p->ctx, "HWC event", err);
		return err;
	}
	/* The first EQ doorbell is valid only after its ID and page arrive. */
	if (changed && (b->init_seen & 1))
		doorbell(b, MANA_HWC_EQ, b->eq_id, b->eq_head % 2048, 1);
	return 0;
}

int mana_hwc_request(struct mana_bootstrap *b, const void *request,
		     size_t request_size, void *response, size_t response_size,
		     size_t *response_length)
{
	const unsigned char *req = request;
	unsigned char *sq, *tx;
	unsigned int off, i;
	int err = 0;
	if (!b || !b->p)
		return -EINVAL;
	if (!b->initialized || b->quarantined || b->error)
		return b->error ? b->error : -ENOTCONN;
	if (b->pending)
		return -EBUSY;
	if (!req || !response || !response_length || request_size < 40 ||
	    request_size > b->max_req || response_size < 32 ||
	    response_size > b->max_resp || mana_get32(req) ||
	    mana_get32(req + 12) != request_size ||
	    !mana_get16(req + 8) || mana_get16(req + 8) > 5 ||
	    mana_get32(req + 16) || mana_get32(req + 20) != mana_get32(req + 4) ||
	    !mana_get16(req + 24) || mana_get16(req + 24) > 3 ||
	    mana_get32(req + 28) != response_size) {
		b->p->error(b->p->ctx, "HWC request size or header", -EINVAL);
		return -EINVAL;
	}
	tx = b->pages[MANA_HWC_TX].addr;
	memcpy(tx, req, request_size);
	tx[10] = tx[11] = 0;
	tx[26] = tx[27] = 0;
	b->pending_type = mana_get32(req + 4);
	b->pending_activity = mana_get32(req + 36);
	b->pending_device = mana_get32(req + 32);
	b->pending_response_version = mana_get16(req + 24);
	b->response = response;
	b->response_size = response_size;
	b->response_length = 0;
	b->rx_done = b->tx_done = 0;
	b->pending = 1;
	sq = b->pages[MANA_HWC_SQ].addr;
	off = (b->sq_head * 32) & 4095;
	memset(sq + off, 0, 64);
	mana_put32(sq + off + 4, 1 | (6U << 8));
	/* Destination RQ and CQ IDs follow the pinned mana_hwc_post_tx_wqe():
	 * tx_oob->vrq_id at WQE+16 and tx_oob->vrcq_id at WQE+20.
	 */
	mana_put32(sq + off + 16, b->pf_dest_vrq_id);
	mana_put32(sq + off + 20, b->pf_dest_vrcq_id);
	mana_put32(sq + off + 24, b->cq_id);
	mana_put32(sq + off + 28, b->sq_id);
	mana_put64(sq + off + 32, b->pages[MANA_HWC_TX].device_addr);
	mana_put32(sq + off + 40, b->mkey);
	mana_put32(sq + off + 44, (uint32_t)request_size);
	b->sq_head += 2;
	doorbell(b, MANA_HWC_SQ, b->sq_id, b->sq_head * 32, 0);
	for (i = 0; i < REQUEST_POLLS; i++) {
		err = mana_bootstrap_poll(b);
		if (err || (b->rx_done && b->tx_done))
			break;
		err = b->p->delay_us(b->p->ctx, 1000);
		if (err)
			break;
	}
	if (!err && !(b->rx_done && b->tx_done))
		err = -ETIMEDOUT;
	if (err) {
		/* A timed-out request still owns its buffers. Do not reuse them. */
		b->error = err;
		b->p->error(b->p->ctx, "HWC request", err);
		return err;
	}
	*response_length = b->response_length;
	b->pending = 0;
	b->response = NULL;
	post_rx(b);
	if (mana_get32((unsigned char *)response + 24)) {
		err = mana_get32((unsigned char *)response + 24) == UINT32_MAX ?
			-EOPNOTSUPP : -EPROTO;
		b->p->error(b->p->ctx, "GDMA response status", err);
		return err;
	}
	return 0;
}

static void request_header(unsigned char *req, uint32_t type,
			   uint32_t req_size, uint32_t resp_size)
{
	mana_put32(req + 4, type);
	mana_put32(req + 8, 1);
	mana_put32(req + 12, req_size);
	mana_put32(req + 20, type);
	mana_put32(req + 24, 1);
	mana_put32(req + 28, resp_size);
}

static int discover(struct mana_bootstrap *b)
{
	unsigned char req[640] = { 0 };
	unsigned char resp[296] = { 0 };
	size_t length;
	unsigned int i, found = 0, count = 0;
	int err;
	request_header(req, 1, sizeof(req), 72);
	mana_put64(req + 40, 1);
	mana_put64(req + 48, 1);
	mana_put32(req + 96, 0x30); /* Protocol OS type: Other. */
	memcpy(req + 128, "Unikraft", 8);
	err = mana_hwc_request(b, req, sizeof(req), resp, 72, &length);
	if (err)
		return err;
	if (length != 72 || mana_get64(resp + 32) != 1)
		return -EPROTONOSUPPORT;
	memset(req, 0, sizeof(req));
	request_header(req, 3, 40, sizeof(resp));
	err = mana_hwc_request(b, req, 40, resp, sizeof(resp), &length);
	if (err)
		return err;
	if (length != sizeof(resp) || mana_get32(resp + 32) > 64)
		return -EPROTO;
	for (i = 0; i < 64; i++) {
		uint32_t id = mana_get32(resp + 40 + i * 4);
		if (!id)
			continue;
		count++;
		if ((id & 0xffff) == 2) {
			if (found++)
				return -EPROTO;
			b->mana_device_id = id;
		}
	}
	if (count != mana_get32(resp + 32) || !found)
		return -ENODEV;
	memset(req, 0, sizeof(req));
	request_header(req, 2, 40, 72);
	err = mana_hwc_request(b, req, 40, resp, 72, &length);
	if (err)
		return err;
	if (length != 72 || mana_get32(resp + 32))
		return -EPROTO;
	b->limits = (struct mana_gdma_limits) {
		.sq = mana_get32(resp + 36), .rq = mana_get32(resp + 40),
		.cq = mana_get32(resp + 44), .eq = mana_get32(resp + 48),
		.doorbells = mana_get32(resp + 52), .mst = mana_get32(resp + 56),
		.cq_mod_contexts = mana_get32(resp + 60),
		.mod_cq = mana_get32(resp + 64), .msix = mana_get32(resp + 68),
	};
	if (!b->limits.sq || !b->limits.rq || !b->limits.cq ||
	    !b->limits.eq || !b->limits.doorbells || !b->limits.msix)
		return -EPROTO;
	return 0;
}

int mana_gdma_start(struct mana_bootstrap *b)
{
	unsigned char msg[32] = { 0 }, req[44] = { 0 }, resp[32];
	unsigned int i, j, before;
	uint16_t high = 0;
	size_t length;
	int err;

	for (i = 0; i < MANA_HWC_PAGE_COUNT; i++) {
		err = b->p->dma_alloc(b->p->ctx, 4096, &b->pages[i]);
		if (err)
			return err;
		if (!b->pages[i].addr || b->pages[i].size != 4096 ||
		    ((uintptr_t)b->pages[i].addr & 4095) ||
		    (b->pages[i].device_addr & 4095) ||
		    b->pages[i].device_addr > UINT64_MAX - 4095)
			return -EPROTO;
		memset(b->pages[i].addr, 0, 4096);
	}
	b->eq_head = 256;
	b->cq_head = 64;
	err = smc_wait(b);
	if (err)
		return err;
	for (i = 0; i < 4; i++) {
		uint64_t frame = b->pages[i].device_addr >> 12;
		for (j = 0; j < 6; j++)
			msg[i * 6 + j] = (unsigned char)(frame >> (8 * j));
		high |= (uint16_t)((frame >> 48) << (i * 4));
	}
	msg[24] = (unsigned char)high;
	msg[25] = (unsigned char)(high >> 8);
	mana_put32(msg + 28, 1);
	b->p->dma_wmb(b->p->ctx);
	err = mana_pci_command(b, (uint16_t)(b->command_saved | 6 | 0x400));
	if (err)
		return err;
	b->dma_exposed = 1;
	for (i = 0; i < 8; i++)
		b->p->write32(b->p->ctx, &b->bar0, b->shm + i * 4,
			      mana_get32(msg + i * 4));
	err = smc_response(b, 1);
	if (err)
		return err;
	for (i = 0; i < HWC_POLLS && !b->initialized; i++) {
		err = mana_bootstrap_poll(b);
		if (err)
			return err;
		if (!b->initialized) {
			err = b->p->delay_us(b->p->ctx, 1000);
			if (err)
				return err;
		}
	}
	if (!b->initialized)
		return -ETIMEDOUT;
	post_rx(b);
	before = b->interrupts;
	request_header(req, 10, sizeof(req), sizeof(resp));
	mana_put32(req + 32, 1); /* HWC device type, instance zero. */
	mana_put32(req + 40, b->eq_id);
	err = mana_hwc_request(b, req, sizeof(req), resp, sizeof(resp), &length);
	if (err)
		return err;
	for (i = 0; i < REQUEST_POLLS; i++) {
		err = mana_bootstrap_poll(b);
		if (err)
			return err;
		if (b->test_event && b->interrupts != before)
			break;
		err = b->p->delay_us(b->p->ctx, 1000);
		if (err)
			return err;
	}
	if (!b->test_event || b->interrupts == before)
		return -ETIMEDOUT;
	err = discover(b);
	if (!err)
		b->ready = 1;
	return err;
}

int mana_gdma_destroy(struct mana_bootstrap *b)
{
	int err;
	if (!b->dma_exposed)
		return 0;
	if (!b->hwc_destroyed) {
		if (!b->destroy_sent) {
			err = smc_wait(b);
			if (err)
				return err;
			b->p->write32(b->p->ctx, &b->bar0, b->shm + 28, 2);
			b->destroy_sent = 1;
		}
		err = smc_response(b, 2);
		if (err)
			return err;
		b->hwc_destroyed = 1;
	}
	err = mana_pci_command(b,
		(uint16_t)((b->command_saved | 2 | 0x400) & ~4U));
	if (err)
		return err;
	b->dma_exposed = 0;
	return 0;
}

int mana_bootstrap_stop(struct mana_bootstrap *b)
{
	unsigned int i;
	int err;
	if (!b || !b->p)
		return -EINVAL;
	if (b->control_owned) {
		b->p->error(b->p->ctx, "bootstrap stop: control still owned", -EBUSY);
		return -EBUSY;
	}
	b->ready = 0;
	err = mana_gdma_destroy(b);
	if (!err)
		err = mana_pci_stop(b);
	if (err) {
		b->quarantined = 1;
		b->p->error(b->p->ctx, "bootstrap stop: resources quarantined", err);
		return err;
	}
	for (i = 0; i < MANA_HWC_PAGE_COUNT; i++) {
		if (b->pages[i].addr)
			b->p->dma_free(b->p->ctx, &b->pages[i]);
		memset(&b->pages[i], 0, sizeof(b->pages[i]));
	}
	b->quarantined = b->initialized = b->pending = 0;
	return 0;
}

int mana_bootstrap_start(struct mana_bootstrap *b, const struct mana_platform *p)
{
	int err;
	if (!b || !p || !p->config_read || !p->config_write || !p->map ||
	    !p->unmap || !p->read32 || !p->write32 || !p->write64 ||
	    !p->dma_alloc || !p->dma_free || !p->dma_rmb || !p->dma_wmb ||
	    !p->irq_alloc || !p->irq_free || !p->delay_us || !p->error)
		return -EINVAL;
	/* Start requires fresh zeroed storage. A failed stop cannot be overwritten. */
	if (b->p)
		return -EBUSY;
	memset(b, 0, sizeof(*b));
	b->p = p;
	if ((p->dma_guarantees & MANA_DMA_REQUIRED) != MANA_DMA_REQUIRED) {
		p->error(p->ctx, "platform DMA guarantees", -ENOTSUP);
		return -ENOTSUP;
	}
	err = mana_pci_start(b);
	if (!err)
		err = mana_gdma_start(b);
	if (err) {
		int stop_err;
		p->error(p->ctx, "bootstrap start", err);
		stop_err = mana_bootstrap_stop(b);
		if (stop_err)
			return stop_err;
	}
	return err;
}
