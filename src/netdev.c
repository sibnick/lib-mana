/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include <uk/mana/netdev.h>
#include <uk/netdev_driver.h>

static struct mana_netdev *context(struct uk_netdev *dev)
{
	return __containerof(dev, struct mana_netdev, dev);
}

static int report(struct mana_netdev *n, const char *stage, int err)
{
	if (n && n->bootstrap && n->bootstrap->p)
		n->bootstrap->p->error(n->bootstrap->p->ctx, stage, err);
	return err;
}

static int available(struct mana_netdev *n)
{
	if (n->shutdown)
		return report(n, "netdev shut down", -ESHUTDOWN);
	if (!n->control.ready || n->control.quarantined ||
	    !n->bootstrap->ready || n->bootstrap->quarantined ||
	    n->bootstrap->error)
		return report(n, "netdev control unavailable", -ENOTCONN);
	return 0;
}

static int probe(struct uk_netdev *dev)
{
	return available(context(dev));
}

static void info(struct uk_netdev *dev, struct uk_netdev_info *i)
{
	struct mana_netdev *n = context(dev);
	*i = (struct uk_netdev_info) {
		.max_rx_queues = 1, .max_tx_queues = 1, .in_queue_pairs = 1,
		.max_mtu = n->max_mtu,
		.ioalign = 1, .features = 0
	};
}

static int configure(struct uk_netdev *dev, const struct uk_netdev_conf *conf)
{
	struct mana_netdev *n = context(dev);
	int err = available(n);
	if (err)
		return err;
	if (!conf || conf->nb_rx_queues != 1 || conf->nb_tx_queues != 1 ||
	    conf->lro)
		return report(n, "netdev configuration", -ENOTSUP);
	if (n->running || n->stopping || n->configured)
		return report(n, "netdev already configured", -EBUSY);
	n->configured = 1;
	return 0;
}

static int queue_info(struct uk_netdev *dev, uint16_t id,
		      struct uk_netdev_queue_info *i)
{
	struct mana_netdev *n = context(dev);
	if (id || !i)
		return report(n, "netdev queue identifier", -EINVAL);
	*i = (struct uk_netdev_queue_info) {
		.nb_max = MANA_NETDEV_DESCRIPTORS, .nb_min = 1, .nb_align = 1,
		.nb_is_power_of_two = 0
	};
	return 0;
}

static int queue_config(struct mana_netdev *n, uint16_t id, uint16_t count)
{
	int err = available(n);
	if (err)
		return err;
	if (!n->configured || id || count > MANA_NETDEV_DESCRIPTORS)
		return report(n, "netdev queue configuration", -EINVAL);
	if (n->running || n->stopping)
		return report(n, "netdev queue busy", -EBUSY);
	return 0;
}

static struct uk_netdev_tx_queue *tx_config(struct uk_netdev *dev,
		uint16_t id, uint16_t count, struct uk_netdev_txqueue_conf *conf)
{
	struct mana_netdev *n = context(dev);
	int err = queue_config(n, id, count);
	if (!err && (!conf || !conf->a))
		err = report(n, "netdev TX allocator", -EINVAL);
	if (!err && n->txq.owner)
		err = report(n, "netdev TX already configured", -EBUSY);
	if (err)
		return ERR2PTR(err);
	n->txq = (struct uk_netdev_tx_queue) {
		.owner = n, .descriptors = count ? count : MANA_NETDEV_DESCRIPTORS
	};
	return &n->txq;
}

static struct uk_netdev_rx_queue *rx_config(struct uk_netdev *dev,
		uint16_t id, uint16_t count, struct uk_netdev_rxqueue_conf *conf)
{
	struct mana_netdev *n = context(dev);
	int err = queue_config(n, id, count);
	if (!err && (!conf || !conf->a || !conf->alloc_rxpkts))
		err = report(n, "netdev RX allocator", -EINVAL);
	if (!err && n->rxq.owner)
		err = report(n, "netdev RX already configured", -EBUSY);
	if (err)
		return ERR2PTR(err);
	n->rxq = (struct uk_netdev_rx_queue) {
		.owner = n, .descriptors = count ? count : MANA_NETDEV_DESCRIPTORS,
		.alloc = conf->alloc_rxpkts, .alloc_ctx = conf->alloc_rxpkts_argp
	};
	return &n->rxq;
}

static const struct uk_hwaddr *mac_get(struct uk_netdev *dev)
{
	return &context(dev)->mac;
}

static uint16_t mtu_get(struct uk_netdev *dev)
{
	return context(dev)->mtu;
}

static int mtu_set(struct uk_netdev *dev, uint16_t mtu)
{
	struct mana_netdev *n = context(dev);
	int err = available(n);
	if (err)
		return err;
	if (n->running || n->stopping)
		return report(n, "netdev MTU busy", -EBUSY);
	err = mana_control_set_mtu(&n->control, mtu);
	if (!err)
		n->mtu = mtu;
	return err;
}

static unsigned int promiscuous_get(struct uk_netdev *dev)
{
	(void)dev;
	return 0;
}

static void complete(void *ctx, const struct mana_tx_result *result)
{
	struct mana_netdev *n = ctx;
	if (result->error)
		report(n, "netdev TX completion", result->error);
	uk_netbuf_free(result->cookie);
}

int mana_netdev_poll(struct mana_netdev *n)
{
	struct mana_tx_result result;
	uint32_t i;
	int err;
	if (!n || !n->bootstrap)
		return -EINVAL;
	err = available(n);
	if (err)
		return err;
	if (!n->running || n->stopping)
		return report(n, "netdev poll stopped", -ESHUTDOWN);
	err = mana_control_poll(&n->control);
	if (err < 0)
		return err;
	for (i = 0; i < n->tx.capacity; i++) {
		err = mana_tx_poll(&n->tx, &result);
		if (err <= 0)
			return err;
		complete(n, &result);
	}
	return 0;
}

static int linear(const struct uk_netbuf *pkt)
{
	uintptr_t offset;
	if (!pkt || pkt->next || pkt->prev || !pkt->data || !pkt->buf ||
	    (uintptr_t)pkt->data < (uintptr_t)pkt->buf)
		return 0;
	offset = (uintptr_t)pkt->data - (uintptr_t)pkt->buf;
	return offset <= pkt->buflen && pkt->len <= pkt->buflen - offset;
}

static int tx_one(struct uk_netdev *dev, struct uk_netdev_tx_queue *q,
		   struct uk_netbuf *pkt)
{
	struct mana_netdev *n = context(dev);
	uint32_t units;
	int err;
	if (q != &n->txq || q->owner != n || !linear(pkt))
		return report(n, "netdev TX linear buffer", -EINVAL);
	if (pkt->flags & (UK_NETBUF_F_PARTIAL_CSUM | UK_NETBUF_F_GSO_TCPV4))
		return report(n, "netdev TX offload", -ENOTSUP);
	err = mana_netdev_poll(n);
	if (err)
		return err;
	err = mana_tx_submit(&n->tx, pkt->data, pkt->len, pkt);
	if (err == -ENOSPC)
		return 0;
	if (err < 0)
		return err;
	units = n->tx.oob_size == 8 ? 1U : 2U;
	return UK_NETDEV_STATUS_SUCCESS |
		(n->tx.count < n->tx.capacity &&
		 units <= n->tx.queue.wq.dma.size / 32 -
			  (n->tx.queue.head - n->tx.queue.tail) ?
		 UK_NETDEV_STATUS_MORE : 0);
}

static int rx_one(struct uk_netdev *dev, struct uk_netdev_rx_queue *q,
		   struct uk_netbuf **out)
{
	struct mana_netdev *n = context(dev);
	struct mana_rx_packet packet = { 0 };
	struct uk_netbuf *pkt = NULL;
	uint32_t i;
	int err, status = 0, refill;
	if (q != &n->rxq || q->owner != n || !out)
		return report(n, "netdev RX queue", -EINVAL);
	*out = NULL;
	err = mana_netdev_poll(n);
	if (err)
		return err;
	refill = mana_rx_refill(&n->rx);
	if (refill && refill != -ENOMEM)
		return refill;
	if (refill)
		status = UK_NETDEV_STATUS_UNDERRUN;
	for (i = 0; i < n->rx.capacity; i++) {
		err = mana_rx_poll(&n->rx, &packet);
		if (err < 0)
			return err;
		if (err == MANA_RX_EMPTY)
			return status;
		if (err == MANA_RX_PACKET)
			break;
		if (err == MANA_RX_FENCE)
			return report(n, "netdev unexpected RX fence", -EPROTO);
		refill = mana_rx_refill(&n->rx);
		if (refill && refill != -ENOMEM)
			return refill;
		if (refill)
			status |= UK_NETDEV_STATUS_UNDERRUN;
	}
	if (!packet.dma.addr)
		return status;
	if (q->alloc(q->alloc_ctx, &pkt, 1) != 1 || !pkt) {
		err = report(n, "netdev RX netbuf allocation", -ENOMEM);
	} else if (!linear(pkt) || pkt->len < packet.length) {
		err = report(n, "netdev RX netbuf capacity", -EMSGSIZE);
	} else {
		memcpy(pkt->data, (unsigned char *)packet.dma.addr + packet.offset,
		       packet.length);
		pkt->len = (uint16_t)packet.length;
		pkt->flags = 0;
		*out = pkt;
		status |= UK_NETDEV_STATUS_SUCCESS;
		err = 0;
	}
	refill = mana_rx_packet_release(&packet);
	if (refill)
		report(n, "netdev RX packet release", refill);
	if (err && pkt)
		uk_netbuf_free(pkt);
	refill = mana_rx_refill(&n->rx);
	if (refill)
		status |= UK_NETDEV_STATUS_UNDERRUN;
	if (err == -ENOMEM)
		return status | UK_NETDEV_STATUS_UNDERRUN;
	if (err)
		return err;
	/* Never revoke an already delivered netbuf after a refill failure. */
	return status;
}

int mana_netdev_stop(struct mana_netdev *n)
{
	int err;
	if (!n)
		return -EINVAL;
	n->running = 0;
	n->stopping = 1;
	if (n->rx.platform) {
		err = mana_rx_stop(&n->rx);
		if (err)
			return err;
	}
	if (n->tx.data) {
		err = mana_tx_stop(&n->tx, complete, n);
		if (err)
			return err;
	}
	if (n->data.control) {
		err = mana_data_stop(&n->data);
		if (err)
			return err;
	}
	n->stopping = 0;
	if (n->registered && n->dev._data->state == UK_NETDEV_RUNNING)
		n->dev._data->state = UK_NETDEV_CONFIGURED;
	return 0;
}

static int start(struct uk_netdev *dev)
{
	struct mana_netdev *n = context(dev);
	int err = available(n), cleanup;
	if (err)
		return err;
	if (n->running)
		return 0;
	if (!n->configured || !n->txq.owner || !n->rxq.owner)
		return report(n, "netdev queues not configured", -EINVAL);
	if (n->stopping || n->data.control || n->tx.data || n->rx.platform)
		return report(n, "netdev cleanup pending", -EBUSY);
	err = mana_data_start(&n->data, &n->control, 4096);
	if (!err)
		err = mana_tx_start(&n->tx, &n->data, n->records,
				    n->txq.descriptors, 4096, 4096);
	if (!err)
		err = mana_rx_start(&n->rx, &n->data, n->slots,
				    n->rxq.descriptors, 4096, 4096);
	if (err) {
		cleanup = mana_netdev_stop(n);
		if (cleanup)
			report(n, "netdev failed start cleanup", cleanup);
		return err;
	}
	n->running = 1;
	return 0;
}

static const struct uk_netdev_ops ops = {
	.probe = probe, .info_get = info, .configure = configure,
	.rxq_info_get = queue_info, .txq_info_get = queue_info,
	.rxq_configure = rx_config, .txq_configure = tx_config,
	.start = start, .hwaddr_get = mac_get, .mtu_get = mtu_get,
	.mtu_set = mtu_set, .promiscuous_get = promiscuous_get
};

int mana_netdev_shutdown(struct mana_netdev *n)
{
	int err;
	if (!n)
		return -EINVAL;
	n->shutdown = 1;
	err = mana_netdev_stop(n);
	if (err)
		return err;
	if (n->control.bootstrap) {
		err = mana_control_stop(&n->control);
		if (err)
			return err;
	}
	if (!n->registered)
		memset(n, 0, sizeof(*n));
	return 0;
}

int mana_netdev_register(struct mana_netdev *n, struct mana_bootstrap *b,
			 struct uk_alloc *alloc)
{
	int err, cleanup;
	if (!n || !b || !b->p || !alloc)
		return -EINVAL;
	if (n->bootstrap)
		return report(n, "netdev already initialized", -EBUSY);
	if (!b->ready || b->error || b->quarantined) {
		b->p->error(b->p->ctx, "netdev bootstrap unavailable", -ENOTCONN);
		return -ENOTCONN;
	}
	n->bootstrap = b;
	n->alloc = alloc;
	err = mana_control_start(&n->control, b, 0);
	if (!err) {
		memcpy(n->mac.addr_bytes, n->control.mac, sizeof(n->mac.addr_bytes));
		n->mtu = n->control.mtu;
		n->max_mtu = n->control.frame_mtu - UK_ETH_HDR_UNTAGGED_LEN;
		n->dev.ops = &ops;
		n->dev.rx_one = rx_one;
		n->dev.tx_one = tx_one;
		err = uk_netdev_drv_register(&n->dev, alloc, "mana");
		if (err >= 0) {
			n->registered = 1;
			return err;
		}
		report(n, "netdev registration", err);
	}
	cleanup = mana_netdev_shutdown(n);
	if (cleanup)
		report(n, "netdev registration cleanup", cleanup);
	return err;
}
