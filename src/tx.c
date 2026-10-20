/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2021, Microsoft Corporation. */
/* Wire formats follow Linux MANA under its BSD-3-Clause license. */
#include "internal.h"
#include <uk/mana/tx.h>

enum {
	TX_OKAY = 32,
	TX_LAST_ERROR = 41,
	ETH_HEADER = 14,
	ETH_MIN_FRAME = 60
};

static const struct mana_platform *platform(const struct mana_tx *tx)
{
	return tx->data->control->bootstrap->p;
}

static int report(struct mana_tx *tx, const char *stage, int err)
{
	if (tx && tx->data && tx->data->control &&
	    tx->data->control->bootstrap) {
		const struct mana_platform *p = platform(tx);
		p->error(p->ctx, stage, err);
	}
	return err;
}

static int poison(struct mana_tx *tx, const char *stage, int err)
{
	struct mana_data *d = tx->data;
	unsigned int i;
	tx->quarantined = 1;
	tx->ready = 0;
	tx->stopped = 1;
	d->quarantined = 1;
	d->ready = 0;
	d->eq.quarantined = 1;
	d->control->quarantined = 1;
	d->control->ready = 0;
	for (i = 0; i < 2; i++)
		if (d->queues[i]) {
			d->queues[i]->stopped = 1;
			d->queues[i]->wq.quarantined = 1;
			d->queues[i]->cq.quarantined = 1;
			d->queues[i]->object.quarantined = 1;
		}
	return report(tx, stage, err);
}

static int usable(struct mana_tx *tx)
{
	struct mana_bootstrap *b;
	if (!tx || !tx->data || !tx->data->control)
		return -EINVAL;
	b = tx->data->control->bootstrap;
	if (tx->quarantined || tx->data->quarantined ||
	    tx->data->control->quarantined || tx->queue.object.quarantined)
		return poison(tx, "TX quarantine", -ENOTCONN);
	if (b->quarantined || b->error || !b->ready)
		return poison(tx, "TX channel unavailable",
			      b->error ? b->error : -ENOTCONN);
	return 0;
}

int mana_tx_start(struct mana_tx *tx, struct mana_data *data,
		  struct mana_tx_record *records, uint32_t capacity,
		  uint32_t wq_size, uint32_t cq_size)
{
	struct mana_control *c;
	int err;
	if (!tx || !data || !data->control || tx->data)
		return -EINVAL;
	tx->data = data;
	c = data->control;
	err = usable(tx);
	if (err)
		return err;
	if (!data->ready || !c->ready || !records || !capacity ||
	    capacity > wq_size / 32 || capacity > cq_size / 64 ||
	    wq_size > 1048576 || cq_size > 1048576 ||
	    c->tx_vport_offset > 0xfff ||
	    c->mtu + ETH_HEADER < ETH_MIN_FRAME) {
		err = report(tx, "TX configuration", -EINVAL);
		tx->data = NULL;
		return err;
	}
	tx->records = records;
	tx->capacity = capacity;
	tx->oob_size = c->short_form_allowed && c->tx_vport_offset <= 0xff ?
		8 : 24;
	memset(records, 0, capacity * sizeof(*records));
	err = mana_data_queue_create(data, &tx->queue, MANA_CONTROL_TX,
				     wq_size, cq_size);
	if (err)
		return err;
	tx->ready = 1;
	return 0;
}

int mana_tx_submit(struct mana_tx *tx, const void *frame, uint32_t length,
		   void *cookie)
{
	unsigned char oob[24] = { 0 };
	struct mana_data_sge sge;
	struct mana_data_work work;
	struct mana_tx_record *record;
	struct mana_control *c;
	const struct mana_platform *p;
	uint32_t wire_length, allocation;
	int err = usable(tx);
	if (err)
		return err;
	c = tx->data->control;
	p = platform(tx);
	if (!tx->ready || tx->stopped)
		return report(tx, "TX stopped", -ESHUTDOWN);
	if (!frame || length < ETH_HEADER || length > (uint32_t)c->mtu + ETH_HEADER)
		return report(tx, "TX frame length", -EMSGSIZE);
	if (!tx->capacity || tx->count > tx->capacity ||
	    tx->first >= tx->capacity ||
	    tx->queue.head - tx->queue.tail > tx->queue.wq.dma.size / 32)
		return poison(tx, "TX FIFO accounting", -EOVERFLOW);
	if (tx->count == tx->capacity ||
	    (tx->oob_size == 8 ? 1U : 2U) >
	    tx->queue.wq.dma.size / 32 - (tx->queue.head - tx->queue.tail))
		return report(tx, "TX full", -ENOSPC);
	record = &tx->records[(tx->first + tx->count) % tx->capacity];
	if (record->dma.addr)
		return poison(tx, "TX FIFO slot owned", -EPROTO);
	wire_length = length < ETH_MIN_FRAME ? ETH_MIN_FRAME : length;
	allocation = (wire_length + 4095) & ~4095U;
	err = p->dma_alloc(p->ctx, allocation, &record->dma);
	if (err)
		return report(tx, "TX DMA allocation", err);
	if (!record->dma.addr || record->dma.size != allocation ||
	    ((uintptr_t)record->dma.addr & 4095) ||
	    (record->dma.device_addr & 4095) ||
	    record->dma.device_addr > UINT64_MAX - (allocation - 1)) {
		p->dma_free(p->ctx, &record->dma);
		memset(record, 0, sizeof(*record));
		return report(tx, "TX DMA contract", -EPROTO);
	}
	memcpy(record->dma.addr, frame, length);
	memset((unsigned char *)record->dma.addr + length, 0, allocation - length);
	mana_put32(oob, (tx->queue.object.cq_id << 8) |
		   (tx->oob_size == 24 ? 1U : 0U));
	if (tx->oob_size == 8)
		mana_put32(oob + 4, (uint32_t)c->tx_vport_offset << 24);
	else
		mana_put32(oob + 12, (uint32_t)c->tx_vport_offset << 16);
	sge = (struct mana_data_sge) {
		.address = record->dma.device_addr,
		.memory_key = c->mkey, .size = wire_length
	};
	work = (struct mana_data_work) {
		.sges = &sge, .num_sge = 1, .data_unit = 0,
		.oob = oob, .oob_size = tx->oob_size
	};
	record->cookie = cookie;
	record->length = length;
	record->wire_length = wire_length;
	err = mana_data_post(&tx->queue, &work, &record->posted);
	if (err) {
		p->dma_free(p->ctx, &record->dma);
		memset(record, 0, sizeof(*record));
		return err;
	}
	tx->count++;
	return 1;
}

int mana_tx_poll(struct mana_tx *tx, struct mana_tx_result *result)
{
	struct mana_data_completion completion;
	struct mana_tx_record *record;
	const struct mana_platform *p;
	uint32_t header, type;
	int err = usable(tx);
	if (err)
		return err;
	if (!result || !tx->queue.object.live)
		return report(tx, "TX poll state", -EINVAL);
	err = mana_control_poll(tx->data->control);
	if (err < 0)
		return poison(tx, "TX control poll", err);
	err = mana_data_poll(tx->data);
	if (err < 0)
		return poison(tx, "TX EQ poll", err);
	err = mana_data_poll_cq(&tx->queue, &completion);
	if (err < 0)
		return poison(tx, "TX CQ poll", err);
	if (!err)
		return 0;
	header = mana_get32(completion.data);
	type = header & 0x3f;
	if (completion.kind != MANA_DATA_COMPLETION || !completion.is_sq ||
	    completion.wq_id != tx->queue.object.wq_id ||
	    (header & 0xc0) != 0x40 || type < TX_OKAY || type > TX_LAST_ERROR)
		return poison(tx, "TX completion type or identifier", -EPROTO);
	if (!tx->capacity || !tx->count || tx->count > tx->capacity ||
	    tx->first >= tx->capacity)
		return poison(tx, "TX completion without FIFO record", -EPROTO);
	record = &tx->records[tx->first];
	if (!record->dma.addr || record->length < ETH_HEADER ||
	    record->length > (uint32_t)tx->data->control->mtu + ETH_HEADER ||
	    record->wire_length != (record->length < ETH_MIN_FRAME ?
				    ETH_MIN_FRAME : record->length) ||
	    record->wire_length > record->dma.size ||
	    record->posted.basic_units != (tx->oob_size == 8 ? 1U : 2U) ||
	    record->posted.offset != ((tx->queue.tail * 32) &
				      ((uint32_t)tx->queue.wq.dma.size - 1)))
		return poison(tx, "TX posted record bounds", -EPROTO);
	err = mana_data_retire(&tx->queue, &record->posted);
	if (err)
		return poison(tx, "TX retirement", err);
	*result = (struct mana_tx_result) {
		.cookie = record->cookie, .length = record->length,
		.wire_length = record->wire_length,
		.type = type, .vendor_error = header >> 8,
		.data_offset = mana_get32(completion.data + 4),
		.sgl_offset = mana_get32(completion.data + 8) & 31,
		.wqe_offset = mana_get32(completion.data + 8) >> 5,
		.error = type == TX_OKAY && !(header >> 8) ? 0 : -EIO
	};
	if (result->error)
		report(tx, "TX packet completion", result->error);
	p = platform(tx);
	p->dma_free(p->ctx, &record->dma);
	memset(record, 0, sizeof(*record));
	tx->first = (tx->first + 1) % tx->capacity;
	tx->count--;
	return 1;
}

int mana_tx_stop(struct mana_tx *tx, mana_tx_complete_fn complete, void *ctx)
{
	struct mana_tx_result result;
	const struct mana_platform *p;
	unsigned int waits;
	int err = usable(tx);
	if (err)
		return err;
	if (tx->count && !complete)
		return report(tx, "TX drain callback required", -EINVAL);
	tx->ready = 0;
	tx->stopped = 1;
	if (tx->queue.data) {
		err = mana_data_queue_quiesce(&tx->queue);
		if (err)
			return poison(tx, "TX quiesce", err);
		p = platform(tx);
		for (waits = 0; tx->count && waits < MANA_TX_DRAIN_POLLS;) {
			err = mana_tx_poll(tx, &result);
			if (err < 0)
				return err;
			if (err) {
				complete(ctx, &result);
			} else {
				waits++;
				err = p->delay_us(p->ctx, 1000);
				if (err)
					return poison(tx, "TX drain clock", err);
			}
		}
		if (tx->count)
			return poison(tx, "TX drain timeout", -ETIMEDOUT);
		if (tx->queue.object.live) {
			err = mana_tx_poll(tx, &result);
			if (err < 0)
				return err;
			if (tx->queue.head != tx->queue.tail)
				return poison(tx, "TX WQ not drained", -EPROTO);
		}
		err = mana_data_queue_destroy(&tx->queue);
		if (err) {
			if (tx->data->quarantined || tx->data->control->quarantined)
				return poison(tx, "TX destroy uncertain", err);
			return err;
		}
	}
	memset(tx, 0, sizeof(*tx));
	return 0;
}
