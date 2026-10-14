/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2021, Microsoft Corporation. */
/* Wire formats follow Linux MANA under its BSD-3-Clause license. */
#include "internal.h"
#include <uk/mana/rx.h>

static int report(struct mana_rx *rx, const char *stage, int err)
{
	if (rx && rx->platform)
		rx->platform->error(rx->platform->ctx, stage, err);
	return err;
}

static int poison(struct mana_rx *rx, const char *stage, int err)
{
	struct mana_data *d = rx->queue.data;
	rx->quarantined = 1;
	rx->ready = 0;
	if (d) {
		d->quarantined = 1;
		d->ready = 0;
		d->control->quarantined = 1;
		d->control->ready = 0;
		rx->queue.object.quarantined = 1;
	}
	return report(rx, stage, err);
}

static int usable(struct mana_rx *rx)
{
	struct mana_data *d;
	if (!rx || !rx->platform || !rx->slots)
		return -EINVAL;
	d = rx->queue.data;
	if (rx->quarantined || (d && (d->quarantined ||
	    d->control->quarantined || d->control->bootstrap->error ||
	    d->control->bootstrap->quarantined)))
		return poison(rx, "RX ownership uncertain", -ENOTCONN);
	return 0;
}

int mana_rx_refill(struct mana_rx *rx)
{
	struct mana_data_sge sge;
	struct mana_data_work work = { .sges = &sge, .num_sge = 1 };
	struct mana_rx_slot *slot;
	size_t size;
	int err = usable(rx);
	if (err)
		return err;
	if (rx->stopping || !rx->queue.object.live)
		return report(rx, "RX refill state", -ESHUTDOWN);
	size = (rx->buffer_size + 4095U) & ~4095U;
	while (rx->count < rx->capacity) {
		slot = &rx->slots[(rx->first + rx->count) % rx->capacity];
		if (slot->dma.addr)
			return poison(rx, "RX slot ownership", -EPROTO);
		err = rx->platform->dma_alloc(rx->platform->ctx, size, &slot->dma);
		if (err)
			return report(rx, "RX buffer allocation", err);
		if (!slot->dma.addr || slot->dma.size != size ||
		    ((uintptr_t)slot->dma.addr & 4095) ||
		    (slot->dma.device_addr & 4095) ||
		    slot->dma.device_addr > UINT64_MAX - (size - 1)) {
			rx->platform->dma_free(rx->platform->ctx, &slot->dma);
			memset(slot, 0, sizeof(*slot));
			return report(rx, "RX buffer DMA bounds", -EINVAL);
		}
		sge = (struct mana_data_sge) {
			.address = slot->dma.device_addr,
			.memory_key = rx->queue.data->control->mkey,
			.size = rx->buffer_size
		};
		err = mana_data_post(&rx->queue, &work, &slot->posted);
		if (err) {
			/* Post reports errors before publication. */
			rx->platform->dma_free(rx->platform->ctx, &slot->dma);
			memset(slot, 0, sizeof(*slot));
			if (rx->queue.data->quarantined)
				return poison(rx, "RX post state", err);
			return report(rx, "RX buffer post", err);
		}
		rx->count++;
	}
	return 0;
}

int mana_rx_start(struct mana_rx *rx, struct mana_data *data,
		  struct mana_rx_slot *slots, uint32_t capacity,
		  uint32_t wq_size, uint32_t cq_size)
{
	uint32_t i;
	int err;
	if (!rx || !data || !data->control || !data->control->bootstrap)
		return -EINVAL;
	if (rx->platform || !slots || !capacity || !data->ready ||
	    capacity > wq_size / 32 || capacity >= cq_size / 64) {
		data->control->bootstrap->p->error(
			data->control->bootstrap->p->ctx, "RX configuration", -EINVAL);
		return -EINVAL;
	}
	for (i = 0; i < capacity; i++)
		if (slots[i].dma.addr) {
			data->control->bootstrap->p->error(
				data->control->bootstrap->p->ctx, "RX slot storage", -EINVAL);
			return -EINVAL;
		}
	rx->platform = data->control->bootstrap->p;
	rx->slots = slots;
	rx->capacity = capacity;
	rx->buffer_size = data->control->mtu + 14U;
	err = mana_data_queue_create(data, &rx->queue, MANA_CONTROL_RX,
				     wq_size, cq_size);
	if (!err)
		err = mana_rx_refill(rx);
	if (!err)
		err = mana_control_set_rx(data->control, 1);
	if (data->quarantined || data->control->quarantined)
		return poison(rx, "RX start uncertain", err ? err : -ENOTCONN);
	if (!err)
		rx->ready = 1;
	return err;
}

int mana_rx_packet_release(struct mana_rx_packet *packet)
{
	if (!packet || !packet->dma.addr || !packet->release)
		return -EINVAL;
	packet->release(packet->release_ctx, &packet->dma);
	memset(packet, 0, sizeof(*packet));
	return 0;
}

int mana_rx_poll(struct mana_rx *rx, struct mana_rx_packet *packet)
{
	struct mana_data_completion c;
	struct mana_rx_slot *slot;
	uint32_t header, type, length, flags;
	int err = usable(rx);
	if (err)
		return err;
	if (!packet || packet->dma.addr || !rx->queue.object.live)
		return report(rx, "RX poll state", -EINVAL);
	err = mana_data_poll(rx->queue.data);
	if (err < 0)
		return poison(rx, "RX data EQ", err);
	err = mana_data_poll_cq(&rx->queue, &c);
	if (err < 0)
		return poison(rx, "RX data CQ", err);
	if (!err)
		return MANA_RX_EMPTY;
	if (c.kind == MANA_DATA_RX_FENCE)
		return MANA_RX_FENCE;
	header = mana_get32(c.data);
	type = header & 0x3f;
	if (c.is_sq || c.wq_id != rx->queue.object.wq_id ||
	    (header & ~0x3fU) != 0x40 || (type != 1 && type != 4))
		return poison(rx, "RX completion type or identifier", -EPROTO);
	if (!rx->count)
		return poison(rx, "RX completion without buffer", -EPROTO);
	slot = &rx->slots[rx->first];
	/* Linux retires RX buffers in FIFO order. It does not use rx_wqe_offset.
	 * Check our posted record against the shared ring before retirement.
	 */
	if (slot->posted.offset != ((rx->queue.tail * 32) &
	    ((uint32_t)rx->queue.wq.dma.size - 1)) ||
	    slot->posted.basic_units != 1 || !slot->dma.addr)
		return poison(rx, "RX posted WQE record", -EPROTO);
	length = mana_get16(c.data + 8);
	flags = mana_get32(c.data + 4);
	if (type == 1) {
		if ((flags & ((1U << 15) | (1U << 31))) ||
		    mana_get16(c.data + 10) || mana_get32(c.data + 12))
			return poison(rx, "RX completion reserved fields", -EPROTO);
	}
	err = mana_data_retire(&rx->queue, &slot->posted);
	if (err)
		return poison(rx, "RX WQE retirement", err);
	rx->first = (rx->first + 1) % rx->capacity;
	rx->count--;
	if (type == 4 || length < 14 || length > rx->buffer_size ||
	    length > slot->dma.size || (flags & (1U << 12)) || rx->stopping) {
		rx->platform->dma_free(rx->platform->ctx, &slot->dma);
		memset(slot, 0, sizeof(*slot));
		rx->dropped++;
		if (!rx->stopping)
			report(rx, "RX packet dropped", -EMSGSIZE);
		return MANA_RX_DROPPED;
	}
	*packet = (struct mana_rx_packet) {
		.dma = slot->dma, .offset = 0, .length = length,
		.release_ctx = rx->platform->ctx,
		.release = rx->platform->dma_free
	};
	memset(slot, 0, sizeof(*slot));
	rx->delivered++;
	return MANA_RX_PACKET;
}

static int fence_poll(void *ctx)
{
	struct mana_rx_packet packet = { 0 };
	int err = mana_rx_poll(ctx, &packet);
	return err < 0 ? err : 0;
}

int mana_rx_stop(struct mana_rx *rx)
{
	struct mana_control_queue *q;
	uint32_t i;
	int err = usable(rx);
	if (err)
		return err;
	rx->ready = 0;
	rx->stopping = 1;
	q = &rx->queue.object;
	if (rx->queue.data) {
		if (q->live) {
			err = mana_control_set_rx(rx->queue.data->control, 0);
			if (!err)
				err = mana_data_queue_quiesce(&rx->queue);
			if (!err && !q->fenced && !q->fence_pending)
				err = mana_control_fence_rx(q);
			if (!err && !q->fenced)
				err = mana_control_wait_fence(q, fence_poll, rx);
			if (err) {
				if (rx->queue.data->control->quarantined ||
				    rx->queue.data->quarantined)
					return poison(rx, "RX shutdown uncertain", err);
				return report(rx, "RX shutdown command", err);
			}
		}
		err = mana_data_queue_destroy(&rx->queue);
		if (err) {
			if (rx->queue.data->quarantined ||
			    rx->queue.data->control->quarantined)
				return poison(rx, "RX destroy uncertain", err);
			return err;
		}
	}
	for (i = 0; i < rx->capacity; i++)
		if (rx->slots[i].dma.addr) {
			rx->platform->dma_free(rx->platform->ctx, &rx->slots[i].dma);
			memset(&rx->slots[i], 0, sizeof(rx->slots[i]));
		}
	memset(rx, 0, sizeof(*rx));
	return 0;
}
