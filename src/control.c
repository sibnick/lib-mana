/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2021, Microsoft Corporation. */
/* Wire formats follow Linux MANA under its BSD-3-Clause license. */
#include "internal.h"
#include <uk/mana/control.h>

enum {
	REGISTER_DEVICE = 4,
	DEREGISTER_DEVICE = 5,
	QUERY_DEVICE = 0x20001,
	CONFIG_TX = 0x20003,
	CREATE_WQ = 0x20004,
	DESTROY_WQ = 0x20005,
	FENCE_RX = 0x20006,
	CONFIG_RX = 0x20007,
	QUERY_PORT = 0x20008,
	QUERY_LINK = 0x2000a
};

static int report(struct mana_control *c, const char *stage, int err)
{
	if (c && c->bootstrap && c->bootstrap->p)
		c->bootstrap->p->error(c->bootstrap->p->ctx, stage, err);
	return err;
}

static int poison(struct mana_control *c, const char *stage, int err)
{
	c->quarantined = 1;
	c->ready = 0;
	if (c->tx)
		c->tx->quarantined = 1;
	if (c->rx)
		c->rx->quarantined = 1;
	return report(c, stage, err);
}

static int usable(struct mana_control *c)
{
	if (!c || !c->bootstrap)
		return -EINVAL;
	if (c->quarantined)
		return report(c, "control quarantine", -ENOTCONN);
	if (!c->bootstrap->ready || c->bootstrap->quarantined ||
	    c->bootstrap->error)
		return poison(c, "control channel",
			      c->bootstrap->error ? c->bootstrap->error : -ENOTCONN);
	return 0;
}

static void header(struct mana_control *c, unsigned char *req, uint32_t code,
		   uint32_t in_size, uint32_t out_size,
		   uint16_t req_version, uint16_t resp_version)
{
	mana_put32(req + 4, code);
	mana_put16(req + 8, req_version);
	mana_put32(req + 12, in_size);
	mana_put32(req + 20, code);
	mana_put16(req + 24, resp_version);
	mana_put32(req + 28, out_size);
	mana_put32(req + 32, c->bootstrap->mana_device_id);
}

static int command(struct mana_control *c, const void *req, size_t in_size,
		    unsigned char *resp, size_t out_size)
{
	size_t length = 0;
	int err = usable(c);
	if (err)
		return err;
	err = mana_hwc_request(c->bootstrap, req, in_size, resp, out_size, &length);
	if (c->bootstrap->error)
		return poison(c, "control request", err);
	if (err)
		return err;
	if (length != out_size)
		return poison(c, "control response length", -EPROTO);
	return 0;
}

int mana_control_poll(struct mana_control *c)
{
	int err = usable(c);
	if (err)
		return err;
	err = mana_bootstrap_poll(c->bootstrap);
	return err ? poison(c, "control event", err) : 0;
}

int mana_control_query_link(struct mana_control *c)
{
	unsigned char req[48] = { 0 }, resp[48] = { 0 };
	int err = usable(c);
	if (err)
		return err;
	if (!c->configured)
		return report(c, "link before port configuration", -ENOTCONN);
	header(c, req, QUERY_LINK, sizeof(req), sizeof(resp), 1, 2);
	mana_put64(req + 40, c->vport);
	err = command(c, req, sizeof(req), resp, sizeof(resp));
	if (!err && resp[36])
		err = report(c, "unconfigured link QoS", -EINVAL);
	c->link_query_error = err;
	if (err) {
		c->link_speed_mbps = c->qos_speed_mbps = 0;
		return err;
	}
	c->qos_speed_mbps = mana_get32(resp + 32);
	c->link_speed_mbps = mana_get32(resp + 40);
	return 0;
}

int mana_control_set_rx(struct mana_control *c, unsigned int enable)
{
	unsigned char req[MANA_GDMA_PAGE_SIZE] = { 0 }, resp[40] = { 0 };
	uint32_t size, i;
	int err = usable(c);
	if (err)
		return err;
	if (!c->configured || enable > 1 ||
	    (enable && (!c->rx || !c->rx->live || c->rx->fence_pending ||
			c->rx->quarantined)))
		return report(c, "RX configuration state", -EINVAL);
	size = 120 + c->indirection_entries * 8;
	if (size > sizeof(req) || size > c->bootstrap->max_req)
		return report(c, "RX steering exceeds HWC request limit", -E2BIG);
	header(c, req, CONFIG_RX, size, sizeof(resp), 5, 2);
	mana_put64(req + 40, c->vport);
	mana_put16(req + 48, (uint16_t)c->indirection_entries);
	mana_put16(req + 50, 120);
	mana_put32(req + 52, enable);
	/* A single-queue table still follows the Linux steering wire shape. */
	req[60] = (unsigned char)enable;
	req[62] = (unsigned char)enable;
	mana_put64(req + 64, enable ? c->rx->handle : MANA_CONTROL_INVALID_HANDLE);
	mana_put16(req + 116, 0x3f); /* MANA_HASH_ENABLE_SUPPORTED, request v5. */
	if (enable)
		for (i = 0; i < c->indirection_entries; i++)
			mana_put64(req + 120 + i * 8, c->rx->handle);
	err = command(c, req, size, resp, sizeof(resp));
	if (err)
		return err;
	c->rx_enabled = enable;
	if (enable)
		c->rx->fenced = 0;
	return 0;
}

int mana_control_start(struct mana_control *c, struct mana_bootstrap *b,
		       uint32_t port_index)
{
	unsigned char req[88] = { 0 }, resp[80] = { 0 };
	uint64_t db_offset;
	unsigned int i, nonzero = 0;
	int err;
	if (!c || !b || !b->p)
		return -EINVAL;
	if (c->bootstrap || b->control_owned) {
		b->p->error(b->p->ctx, "control already owned", -EBUSY);
		return -EBUSY;
	}
	if (!b->ready || b->error || b->quarantined) {
		err = b->error ? b->error : -ENOTCONN;
		b->p->error(b->p->ctx, "control bootstrap", err);
		return err;
	}
	c->bootstrap = b;
	b->control_owned = 1;
	header(c, req, REGISTER_DEVICE, 40, 44, 1, 1);
	err = command(c, req, 40, resp, 44);
	if (err)
		return err;
	c->registered = 1;
	c->pd_id = mana_get32(resp + 32);
	c->mkey = mana_get32(resp + 36);
	c->doorbell_id = mana_get32(resp + 40);
	db_offset = b->db + (uint64_t)b->db_size * c->doorbell_id;
	if (c->pd_id == UINT32_MAX || c->mkey == UINT32_MAX ||
	    !mana_range(b->bar0.size, b->db,
			((uint64_t)c->doorbell_id + 1) * b->db_size) ||
	    mana_overlap(db_offset, b->db_size, b->shm, 32) ||
	    (!b->table_bir && mana_overlap(db_offset, b->db_size,
					  b->table_offset, b->table_length)) ||
	    (!b->pba_bir && mana_overlap(db_offset, b->db_size,
					b->pba_offset, b->pba_length)))
		return report(c, "registered device resources", -EPROTO);
	memset(req, 0, sizeof(req));
	header(c, req, QUERY_DEVICE, sizeof(req), sizeof(resp), 1, 3);
	mana_put32(req + 76, 1); /* MANA protocol 0.1.1, no driver capabilities. */
	mana_put32(req + 80, 1);
	err = command(c, req, sizeof(req), resp, sizeof(resp));
	if (err)
		return err;
	for (i = 0; i < 4; i++)
		c->capabilities[i] = mana_get64(resp + 32 + i * 8);
	c->max_ports = mana_get16(resp + 64);
	c->max_eqs = mana_get32(resp + 68);
	c->frame_mtu = mana_get16(resp + 8) >= 2 ?
		mana_get16(resp + 72) : 0;
	if (!c->frame_mtu) {
		c->frame_mtu = 1514;
		report(c, "legacy adapter MTU: use 1514-byte frame limit", -EOPNOTSUPP);
	}
	if (c->frame_mtu < 82 || !c->max_ports || !c->max_eqs ||
	    (mana_get16(resp + 8) >= 3 && resp[66]))
		return report(c, "unsupported device configuration", -EPROTO);
	if (port_index >= c->max_ports)
		return report(c, "port index", -EINVAL);
	memset(req, 0, sizeof(req));
	header(c, req, QUERY_PORT, 44, 64, 1, 1);
	mana_put32(req + 40, port_index);
	err = command(c, req, 44, resp, 64);
	if (err)
		return err;
	c->max_sq = mana_get32(resp + 32);
	c->max_rq = mana_get32(resp + 36);
	c->indirection_entries = mana_get32(resp + 40);
	if (!c->indirection_entries || c->indirection_entries > 512 ||
	    (c->indirection_entries & (c->indirection_entries - 1))) {
		c->indirection_entries = 64;
		report(c, "legacy indirection size: use 64 entries", -EOPNOTSUPP);
	}
	if (120 + c->indirection_entries * 8 > b->max_req)
		return report(c, "RX steering exceeds HWC request limit", -E2BIG);
	memcpy(c->mac, resp + 48, sizeof(c->mac));
	c->vport = mana_get64(resp + 56);
	for (i = 0; i < sizeof(c->mac); i++)
		nonzero |= c->mac[i];
	if (!c->max_sq || !c->max_rq || !nonzero || (c->mac[0] & 1) ||
	    c->vport == MANA_CONTROL_INVALID_HANDLE)
		return report(c, "vPort configuration", -EPROTO);
	c->mtu = c->frame_mtu - 14 < 1500 ? c->frame_mtu - 14 : 1500;
	memset(req, 0, sizeof(req));
	header(c, req, CONFIG_TX, 56, 36, 1, 1);
	mana_put64(req + 40, c->vport);
	mana_put32(req + 48, c->pd_id);
	mana_put32(req + 52, c->doorbell_id);
	err = command(c, req, 56, resp, 36);
	if (err)
		return err;
	c->configured = 1;
	c->tx_vport_offset = mana_get16(resp + 32);
	c->short_form_allowed = resp[34];
	if (c->short_form_allowed > 1)
		return poison(c, "TX configuration response", -EPROTO);
	err = mana_control_set_rx(c, 0);
	if (err)
		return err;
	err = mana_control_query_link(c);
	if (err && err != -EOPNOTSUPP)
		return err;
	c->ready = 1;
	return 0;
}

int mana_control_set_mtu(struct mana_control *c, uint16_t mtu)
{
	int err = usable(c);
	if (err)
		return err;
	if (!c->ready || mtu < 68 || (uint32_t)mtu + 14 > c->frame_mtu)
		return report(c, "MTU range", -EINVAL);
	if (c->tx || c->rx || c->rx_enabled)
		return report(c, "MTU while queues exist", -EBUSY);
	c->mtu = mtu;
	return 0;
}

int mana_control_create_queue(struct mana_control *c,
			      struct mana_control_queue *q,
			      enum mana_control_queue_type type,
			      const struct mana_control_queue_spec *spec)
{
	unsigned char req[96] = { 0 }, resp[56] = { 0 };
	struct mana_control_queue **slot;
	int err = usable(c);
	if (err)
		return err;
	if (!c->ready || !q || !spec || q->control ||
	    (type != MANA_CONTROL_TX && type != MANA_CONTROL_RX))
		return report(c, "queue create state", -EINVAL);
	slot = type == MANA_CONTROL_TX ? &c->tx : &c->rx;
	if (*slot)
		return report(c, "queue pair limit", -ENOSPC);
	if (!spec->wq_region || !spec->cq_region ||
	    spec->wq_region == spec->cq_region ||
	    spec->wq_size < 4096 || (spec->wq_size & (spec->wq_size - 1)) ||
	    spec->cq_size < 4096 || (spec->cq_size & (spec->cq_size - 1)) ||
	    spec->parent_eq > 0xffffff || spec->parent_eq == c->bootstrap->eq_id)
		return report(c, "queue region or size", -EINVAL);
	{
		const struct mana_control_queue *other =
			type == MANA_CONTROL_TX ? c->rx : c->tx;
		if (other &&
		    (spec->wq_region == other->spec.wq_region ||
		     spec->wq_region == other->spec.cq_region ||
		     spec->cq_region == other->spec.wq_region ||
		     spec->cq_region == other->spec.cq_region))
			return report(c, "queue region already consumed", -EINVAL);
	}
	header(c, req, CREATE_WQ, sizeof(req), sizeof(resp), 3, 2);
	mana_put64(req + 40, c->vport);
	mana_put32(req + 48, type);
	mana_put64(req + 56, spec->wq_region);
	mana_put64(req + 64, spec->cq_region);
	mana_put32(req + 72, spec->wq_size);
	mana_put32(req + 76, spec->cq_size);
	mana_put32(req + 80, spec->moderation_context);
	mana_put32(req + 84, spec->parent_eq);
	/* Preserve region ownership if the command result becomes uncertain. */
	q->control = c;
	q->type = type;
	q->spec = *spec;
	q->handle = MANA_CONTROL_INVALID_HANDLE;
	*slot = q;
	err = command(c, req, sizeof(req), resp, sizeof(resp));
	if (err) {
		if (c->quarantined)
			q->quarantined = 1;
		else {
			*slot = NULL;
			memset(q, 0, sizeof(*q));
		}
		return err;
	}
	q->handle = mana_get64(resp + 40);
	q->wq_id = mana_get32(resp + 32);
	q->cq_id = mana_get32(resp + 36);
	q->live = 1;
	if (q->handle == MANA_CONTROL_INVALID_HANDLE || q->wq_id > 0xffffff ||
	    q->cq_id >= c->bootstrap->max_cqs ||
	    q->cq_id == c->bootstrap->cq_id ||
	    q->wq_id == (type == MANA_CONTROL_TX ?
			 c->bootstrap->sq_id : c->bootstrap->rq_id) ||
	    (c->tx && c->rx && (c->tx->cq_id == c->rx->cq_id ||
				c->tx->handle == c->rx->handle)))
		return poison(c, "created queue identifiers", -EPROTO);
	return 0;
}

int mana_control_fence_rx(struct mana_control_queue *q)
{
	unsigned char req[48] = { 0 }, resp[32] = { 0 };
	struct mana_control *c = q ? q->control : NULL;
	int err = usable(c);
	if (err)
		return err;
	if (q != c->rx || !q->live || c->rx_enabled || q->fence_pending)
		return report(c, "RX fence state", -EINVAL);
	header(c, req, FENCE_RX, sizeof(req), sizeof(resp), 1, 1);
	mana_put64(req + 40, q->handle);
	q->fenced = 0;
	q->fence_pending = 1;
	err = command(c, req, sizeof(req), resp, sizeof(resp));
	if (err && !c->quarantined)
		q->fence_pending = 0;
	return err;
}

int mana_control_fence_complete(struct mana_control_queue *q)
{
	struct mana_control *c = q ? q->control : NULL;
	int err = usable(c);
	if (err)
		return err;
	if (q != c->rx || !q->live || !q->fence_pending || q->quarantined)
		return report(c, "RX fence completion state", -EPROTO);
	q->fence_pending = 0;
	q->fenced = 1;
	return 0;
}

int mana_control_wait_fence(struct mana_control_queue *q,
			    int (*poll)(void *), void *ctx)
{
	struct mana_control *c = q ? q->control : NULL;
	unsigned int i;
	int err = usable(c);
	if (err)
		return err;
	if (!poll || q != c->rx || !q->live ||
	    (!q->fence_pending && !q->fenced))
		return report(c, "RX fence wait state", -EINVAL);
	for (i = 0; i < 10000; i++) {
		if (q->fenced)
			return 0;
		err = mana_control_poll(c);
		if (!err)
			err = poll(ctx);
		if (err)
			return poison(c, "RX fence poll", err);
		if (q->fenced)
			return 0;
		err = c->bootstrap->p->delay_us(c->bootstrap->p->ctx, 1000);
		if (err)
			return poison(c, "RX fence clock", err);
	}
	return poison(c, "RX fence timeout", -ETIMEDOUT);
}

int mana_control_destroy_queue(struct mana_control_queue *q)
{
	unsigned char req[56] = { 0 }, resp[32] = { 0 };
	struct mana_control *c = q ? q->control : NULL;
	int err = usable(c);
	if (err)
		return err;
	if (!q->live || q->quarantined ||
	    (q != c->tx && q != c->rx) ||
	    (q->type == MANA_CONTROL_RX &&
	     (c->rx_enabled || !q->fenced || q->fence_pending)))
		return report(c, "queue destroy state", -EBUSY);
	header(c, req, DESTROY_WQ, sizeof(req), sizeof(resp), 1, 1);
	mana_put32(req + 40, q->type);
	mana_put64(req + 48, q->handle);
	err = command(c, req, sizeof(req), resp, sizeof(resp));
	if (err)
		return err;
	if (q == c->tx)
		c->tx = NULL;
	else
		c->rx = NULL;
	memset(q, 0, sizeof(*q));
	return 0;
}

int mana_control_stop(struct mana_control *c)
{
	unsigned char req[40] = { 0 }, resp[32] = { 0 };
	int err = usable(c);
	if (err)
		return err;
	if (c->tx || c->rx || c->data_owned)
		return report(c, "control queues still owned", -EBUSY);
	if (c->configured) {
		err = mana_control_set_rx(c, 0);
		if (err)
			return err;
	}
	if (c->registered) {
		header(c, req, DEREGISTER_DEVICE, sizeof(req), sizeof(resp), 1, 1);
		err = command(c, req, sizeof(req), resp, sizeof(resp));
		if (err)
			return err;
	}
	c->bootstrap->control_owned = 0;
	memset(c, 0, sizeof(*c));
	return 0;
}
