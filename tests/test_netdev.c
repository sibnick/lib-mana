/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include "host/bootstrap.h"
#include "../src/internal.h"
#include <uk/mana/netdev.h>
#include <stdlib.h>

struct fixture {
	struct host_bootstrap model;
	struct mana_netdev n;
	uint32_t regions, eq_producer, tx_producer, rx_producer;
	uint32_t fail_code, fail_occurrence;
	unsigned int auto_tx, no_fence, creates, destroys, short_form;
	void (*write64)(void *, const struct mana_mapping *, uint64_t, uint64_t);
	unsigned int rx_alloc_fail, rx_capacity;
};

/* Registered devices cannot leave the pinned core's list. */
static struct fixture fixtures[16];
static unsigned int fixture_count, malloc_fail, packet_frees, buffer_allocs;

static void *allocate(struct uk_alloc *a, size_t size)
{
	(void)a;
	if (malloc_fail) {
		malloc_fail = 0;
		return NULL;
	}
	return malloc(size);
}

static void *allocate_zero(struct uk_alloc *a, size_t count, size_t size)
{
	void *p = allocate(a, count * size);
	if (p)
		memset(p, 0, count * size);
	return p;
}

static void *allocate_aligned(struct uk_alloc *a, size_t align, size_t size)
{
	(void)a;
	if (malloc_fail) {
		malloc_fail = 0;
		return NULL;
	}
	return aligned_alloc(align, (size + align - 1) & ~(align - 1));
}

static void release(struct uk_alloc *a, void *p)
{
	(void)a;
	free(p);
}

static struct uk_alloc allocator = {
	.malloc = allocate, .calloc = allocate_zero,
	.memalign = allocate_aligned, .free = release
};

static void packet_dtor(struct uk_netbuf *pkt)
{
	(void)pkt;
	packet_frees++;
}

static struct uk_netbuf *packet(unsigned int length)
{
	struct uk_netbuf *pkt = uk_netbuf_alloc_buf(&allocator, 2048, 8, 64,
						    16, packet_dtor);
	if (pkt) {
		pkt->len = (uint16_t)length;
		memset(pkt->data, 0x7a, length);
		memset(pkt->priv, 0x5a, 16);
		buffer_allocs++;
	}
	return pkt;
}

static uint16_t rx_allocate(void *ctx, struct uk_netbuf **out, uint16_t count)
{
	struct fixture *f = ctx;
	if (count != 1)
		abort();
	if (f->rx_alloc_fail)
		return 0;
	*out = packet(f->rx_capacity);
	return *out ? 1 : 0;
}

static void tx_completion(struct fixture *f, uint32_t type)
{
	struct mana_data_queue *q = &f->n.tx.queue;
	uint32_t entries = (uint32_t)q->cq.dma.size / 64;
	unsigned char *c = (unsigned char *)q->cq.dma.addr +
		(f->tx_producer % entries) * 64;
	memset(c, 0, 64);
	mana_put32(c, 0x40 | type);
	mana_put32(c + 60, q->object.wq_id | (1U << 24) |
		   ((f->tx_producer / entries) & 7) << 29);
	f->tx_producer++;
}

static void rx_completion(struct fixture *f, uint32_t type, uint32_t length)
{
	struct mana_data_queue *q = &f->n.rx.queue;
	uint32_t entries = (uint32_t)q->cq.dma.size / 64;
	unsigned char *c = (unsigned char *)q->cq.dma.addr +
		(f->rx_producer % entries) * 64;
	memset(c, 0, 64);
	mana_put32(c, 0x40 | type);
	mana_put16(c + 8, (uint16_t)length);
	mana_put32(c + 60, q->object.wq_id |
		   ((f->rx_producer / entries) & 7) << 29);
	f->rx_producer++;
}

static int respond(void *ctx, const unsigned char *req, size_t size,
		   unsigned char *resp, size_t capacity, size_t *length)
{
	struct fixture *f = ctx;
	uint32_t code = mana_get32(req + 4), type, i;
	(void)size;
	(void)capacity;
	(void)length;
	if (code == 1 || code == 2 || code == 3 ||
	    (code == 10 && mana_get32(req + 32) == 1))
		return -ENOTSUP;
	if (code == f->fail_code && --f->fail_occurrence == 0) {
		mana_put32(resp + 24, 1);
		f->fail_code = 0;
		return 0;
	}
	switch (code) {
	case 4:
		mana_put32(resp + 32, 23);
		mana_put32(resp + 36, 0x91);
		break;
	case 5:
	case 0x20007:
	case 0x2000a:
	case 27:
	case 13:
		break;
	case 0x20003:
		if (f->short_form)
			resp[34] = 1;
		break;
	case 0x20001:
		mana_put16(resp + 64, 1);
		mana_put32(resp + 68, 4);
		mana_put16(resp + 72, 9014);
		break;
	case 0x20008:
		mana_put32(resp + 32, 2);
		mana_put32(resp + 36, 2);
		mana_put32(resp + 40, 64);
		memcpy(resp + 48, "\x02\x11\x22\x33\x44\x55", 6);
		mana_put64(resp + 56, 0x12345678);
		break;
	case 25:
		mana_put64(resp + 32, 0x12340001 + f->regions++);
		break;
	case 12:
		mana_put32(resp + 32, 17);
		f->eq_producer = 256;
		break;
	case 10: {
		unsigned char *c = (unsigned char *)f->n.data.eq.dma.addr +
			(f->eq_producer % 256) * 16;
		memset(c, 0, 16);
		mana_put32(c + 12, 64 | ((f->eq_producer / 256) & 7) << 29);
		f->eq_producer++;
		break;
	}
	case 0x20004:
		type = mana_get32(req + 48);
		if (type != MANA_CONTROL_TX && type != MANA_CONTROL_RX)
			abort();
		mana_put32(resp + 32, type == MANA_CONTROL_TX ? 21 : 22);
		mana_put32(resp + 36, type == MANA_CONTROL_TX ? 11 : 12);
		mana_put64(resp + 40, 0x56780000 + type);
		if (type == MANA_CONTROL_TX)
			f->tx_producer = mana_get32(req + 76) / 64;
		else
			f->rx_producer = mana_get32(req + 76) / 64;
		f->creates++;
		break;
	case 0x20006:
		if (!f->no_fence)
			rx_completion(f, 3, 0);
		if (f->auto_tx)
			for (i = 0; i < f->n.tx.count; i++)
				tx_completion(f, i ? 33 : 32);
		break;
	case 0x20005:
		f->destroys++;
		break;
	default:
		abort();
	}
	return 0;
}

static void write64(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint64_t value)
{
	struct fixture *f = ctx;
	uint32_t id = (uint32_t)value & 0xffffff;
	if (id == 5 || id == 6 || id == 7 || id == 9) {
		f->write64(ctx, m, off, value);
		return;
	}
	if (host_mock_write(&f->model.host, (size_t)off, 8, value))
		abort();
}

static struct fixture *fresh(void)
{
	struct fixture *f;
	if (fixture_count == sizeof(fixtures) / sizeof(fixtures[0]))
		abort();
	f = &fixtures[fixture_count++];
	host_bootstrap_init(&f->model, HOST_BOOTSTRAP_NORMAL);
	f->model.respond = respond;
	f->model.respond_ctx = f;
	f->write64 = f->model.platform.write64;
	f->model.platform.write64 = write64;
	f->rx_capacity = 1800;
	f->auto_tx = 1;
	return f;
}

static int register_device(struct fixture *f)
{
	unsigned int count = uk_netdev_count();
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) ==
		   (int)count);
	TEST_CHECK(uk_netdev_count() == count + 1);
	TEST_CHECK(uk_netdev_get(count) == &f->n.dev);
	TEST_CHECK(uk_netdev_probe(&f->n.dev) == 0);
	return 0;
}

static int configure_device(struct fixture *f, uint16_t descriptors)
{
	struct uk_netdev_conf conf = { .nb_rx_queues = 1, .nb_tx_queues = 1 };
	struct uk_netdev_txqueue_conf tx = { .a = &allocator };
	struct uk_netdev_rxqueue_conf rx = {
		.a = &allocator, .alloc_rxpkts = rx_allocate,
		.alloc_rxpkts_argp = f
	};
	TEST_CHECK(uk_netdev_configure(&f->n.dev, &conf) == 0);
	TEST_CHECK(uk_netdev_txq_configure(&f->n.dev, 0, descriptors, &tx) == 0);
	TEST_CHECK(uk_netdev_rxq_configure(&f->n.dev, 0, descriptors, &rx) == 0);
	return 0;
}

static int finish(struct fixture *f)
{
	TEST_CHECK(mana_netdev_shutdown(&f->n) == 0);
	TEST_CHECK(mana_netdev_shutdown(&f->n) == 0);
	TEST_CHECK(f->n.registered && f->n.shutdown && !f->n.running);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == 0);
	TEST_CHECK(!f->model.host.live_allocations);
	TEST_CHECK(!f->model.mismatch);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == 0);
	return 0;
}

static int dma_gate(void)
{
	struct fixture *f = fresh();
	unsigned int count = uk_netdev_count();
	f->model.platform.dma_guarantees = 0;
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) ==
		   -ENOTSUP);
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) ==
		   -ENOTCONN);
	TEST_CHECK(uk_netdev_count() == count && !f->n.registered);
	TEST_CHECK(!f->model.host.live_allocations);
	return 0;
}

static int registration_errors(void)
{
	struct fixture *f = fresh();
	unsigned int count = uk_netdev_count();
	TEST_CHECK(mana_bootstrap_start(&f->model.b, &f->model.platform) == 0);
	f->fail_code = 4;
	f->fail_occurrence = 1;
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) == -EPROTO);
	TEST_CHECK(!f->n.bootstrap && !f->model.b.control_owned);
	malloc_fail = 1;
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) == -ENOMEM);
	TEST_CHECK(!f->n.bootstrap && !f->model.b.control_owned);
	TEST_CHECK(uk_netdev_count() == count);
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) ==
		   (int)count);
	TEST_CHECK(mana_netdev_register(&f->n, &f->model.b, &allocator) == -EBUSY);
	return finish(f);
}

static int lifecycle(void)
{
	struct fixture *f = fresh();
	struct uk_netdev_info info;
	struct uk_netdev_queue_info qi;
	struct uk_netdev_conf bad = { .nb_rx_queues = 2, .nb_tx_queues = 1 };
	struct uk_netbuf *pkt;
	unsigned int i, freed = packet_frees;
	TEST_CHECK(register_device(f) == 0);
	uk_netdev_info_get(&f->n.dev, &info);
	TEST_CHECK(info.max_rx_queues == 1 && info.max_tx_queues == 1);
	TEST_CHECK(info.in_queue_pairs && info.max_mtu == 9000 && !info.features);
	TEST_CHECK(!info.nb_encap_tx && !info.nb_encap_rx && info.ioalign == 1);
	TEST_CHECK(uk_netdev_configure(&f->n.dev, &bad) == -EINVAL);
	TEST_CHECK(f->n.dev.ops->configure(&f->n.dev, &bad) == -ENOTSUP);
	bad.nb_rx_queues = 1;
	bad.lro = 1;
	TEST_CHECK(uk_netdev_configure(&f->n.dev, &bad) == -ENOTSUP);
	TEST_CHECK(configure_device(f, 0) == 0);
	TEST_CHECK(uk_netdev_mtu_get(&f->n.dev) == 1500);
	TEST_CHECK(!memcmp(uk_netdev_hwaddr_get(&f->n.dev)->addr_bytes,
			   "\x02\x11\x22\x33\x44\x55", 6));
	TEST_CHECK(uk_netdev_txq_info_get(&f->n.dev, 0, &qi) == 0);
	TEST_CHECK(qi.nb_min == 1 && qi.nb_max == 32 && !qi.nb_is_power_of_two);
	TEST_CHECK(uk_netdev_rxq_info_get(&f->n.dev, 0, &qi) == 0);
	TEST_CHECK(f->n.dev.ops->txq_info_get(&f->n.dev, 1, &qi) == -EINVAL);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(f->n.rx.count == 32 && f->n.tx.capacity == 32);
	for (i = 0; i < 32; i++) {
		pkt = packet(60);
		TEST_CHECK(pkt);
		TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, pkt) ==
			   (UK_NETDEV_STATUS_SUCCESS |
			    (i < 31 ? UK_NETDEV_STATUS_MORE : 0)));
	}
	TEST_CHECK(f->model.host.live_allocations == 75);
	TEST_CHECK(f->n.dev.ops->start(&f->n.dev) == 0);
	TEST_CHECK(uk_netdev_mtu_set(&f->n.dev, 1400) == -EBUSY);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(packet_frees == freed + 32);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(uk_netdev_state_get(&f->n.dev) == UK_NETDEV_CONFIGURED);
	TEST_CHECK(uk_netdev_mtu_set(&f->n.dev, 1400) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(f->n.rx.buffer_size == 1414);
	TEST_CHECK(!f->model.b.link_known && !f->model.b.link_up);
	TEST_CHECK(uk_netdev_hwaddr_set(&f->n.dev, &f->n.mac) == -ENOTSUP);
	TEST_CHECK(uk_netdev_promiscuous_set(&f->n.dev, 1) == -ENOTSUP);
	TEST_CHECK(uk_netdev_promiscuous_get(&f->n.dev) == 0);
	TEST_CHECK(finish(f) == 0);
	uk_netdev_info_get(&f->n.dev, &info);
	TEST_CHECK(info.max_mtu == 9000 && uk_netdev_mtu_get(&f->n.dev) == 1400);
	TEST_CHECK(f->n.dev.ops->start(&f->n.dev) == -ESHUTDOWN);
	return 0;
}

static int tx_ownership(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *a, *b, *c;
	unsigned int freed = packet_frees;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	a = packet(14);
	b = packet(60);
	c = packet(60);
	TEST_CHECK(a && b && c);
	a->flags = UK_NETBUF_F_PARTIAL_CSUM;
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, a) == -ENOTSUP);
	a->flags = UK_NETBUF_F_GSO_TCPV4;
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, a) == -ENOTSUP);
	a->flags = 0;
	a->next = b;
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, a) == -EINVAL);
	a->next = NULL;
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, a) ==
		   (UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_MORE));
	TEST_CHECK(f->n.records[0].cookie == a && f->n.records[0].dma.addr != a->data);
	TEST_CHECK(!memcmp(f->n.records[0].dma.addr, a->data, 14));
	TEST_CHECK(((unsigned char *)f->n.records[0].dma.addr)[59] == 0);
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, b) == UK_NETDEV_STATUS_SUCCESS);
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, c) == 0);
	TEST_CHECK(packet_frees == freed && uk_refcount_read(&c->refcount) == 1);
	tx_completion(f, 32);
	tx_completion(f, 33);
	TEST_CHECK(mana_netdev_poll(&f->n) == 0);
	TEST_CHECK(packet_frees == freed + 2 && !f->n.tx.count);
	TEST_CHECK(mana_netdev_poll(&f->n) == 0 && packet_frees == freed + 2);
	c->len = 13;
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, c) == -EMSGSIZE);
	c->len = 60;
	uk_netbuf_ref(c);
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, c) & UK_NETDEV_STATUS_SUCCESS);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(packet_frees == freed + 2 && uk_refcount_read(&c->refcount) == 1);
	uk_netbuf_free(c);
	TEST_CHECK(packet_frees == freed + 3);
	return finish(f);
}

static int rx_ownership(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *out = NULL;
	unsigned int freed = packet_frees;
	int status;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	memset(f->n.slots[0].dma.addr, 0x37, 64);
	rx_completion(f, 1, 64);
	TEST_CHECK(uk_netdev_rx_one(&f->n.dev, 0, &out) == UK_NETDEV_STATUS_SUCCESS);
	TEST_CHECK(out && out->len == 64 && !out->flags);
	TEST_CHECK((unsigned char *)out->data - (unsigned char *)out->buf == 64);
	TEST_CHECK(((unsigned char *)out->data)[63] == 0x37);
	TEST_CHECK(((unsigned char *)out->priv)[0] == 0x5a);
	TEST_CHECK(f->n.rx.count == 2);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0 && packet_frees == freed);
	TEST_CHECK(((unsigned char *)out->data)[63] == 0x37);
	uk_netbuf_free(out);
	TEST_CHECK(packet_frees == freed + 1);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	f->rx_capacity = 59;
	rx_completion(f, 1, 60);
	out = NULL;
	TEST_CHECK(uk_netdev_rx_one(&f->n.dev, 0, &out) == -EMSGSIZE);
	TEST_CHECK(!out && f->n.rx.count == 2 && packet_frees == freed + 2);
	f->rx_capacity = 1800;
	f->rx_alloc_fail = 1;
	rx_completion(f, 1, 60);
	TEST_CHECK(uk_netdev_rx_one(&f->n.dev, 0, &out) ==
		   UK_NETDEV_STATUS_UNDERRUN);
	TEST_CHECK(!out && f->n.rx.count == 2);
	f->rx_alloc_fail = 0;
	rx_completion(f, 1, 60);
	TEST_CHECK(host_fault_arm(&f->model.host, HOST_FAULT_ALLOC, 1, -ENOMEM) == 0);
	status = uk_netdev_rx_one(&f->n.dev, 0, &out);
	TEST_CHECK(status == (UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_UNDERRUN));
	TEST_CHECK(out && out->len == 60 && f->n.rx.count == 1);
	uk_netbuf_free(out);
	out = NULL;
	TEST_CHECK(uk_netdev_rx_one(&f->n.dev, 0, &out) == 0);
	TEST_CHECK(f->n.rx.count == 2 && !out);
	return finish(f);
}

static int partial_start(void)
{
	struct fixture *f = fresh();
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	f->fail_code = 0x20004;
	f->fail_occurrence = 2;
	TEST_CHECK(uk_netdev_start(&f->n.dev) == -EPROTO);
	TEST_CHECK(!f->n.running && !f->n.stopping && !f->n.data.control);
	TEST_CHECK(f->model.host.live_allocations == 6);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	f->fail_code = 0x20007;
	f->fail_occurrence = 1;
	TEST_CHECK(uk_netdev_start(&f->n.dev) == -EPROTO);
	TEST_CHECK(!f->n.stopping && !f->n.data.control);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	return finish(f);
}

static int retry_stop(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt;
	unsigned int freed = packet_frees;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	pkt = packet(60);
	TEST_CHECK(pkt && (uk_netdev_tx_one(&f->n.dev, 0, pkt) & UK_NETDEV_STATUS_SUCCESS));
	f->fail_code = 0x20005;
	f->fail_occurrence = 1;
	TEST_CHECK(mana_netdev_stop(&f->n) == -EPROTO);
	TEST_CHECK(f->n.stopping && !f->n.running && f->n.tx.count == 1);
	TEST_CHECK(f->n.dev.ops->start(&f->n.dev) == -EBUSY);
	TEST_CHECK(mana_netdev_poll(&f->n) == -ESHUTDOWN);
	TEST_CHECK(packet_frees == freed);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(packet_frees == freed + 1);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	return finish(f);
}

static int uncertain_stop(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt;
	unsigned int freed = packet_frees;
	size_t retained;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	pkt = packet(60);
	TEST_CHECK(pkt && (uk_netdev_tx_one(&f->n.dev, 0, pkt) & UK_NETDEV_STATUS_SUCCESS));
	f->auto_tx = 0;
	TEST_CHECK(mana_netdev_stop(&f->n) == -ETIMEDOUT);
	TEST_CHECK(f->n.tx.count == 1 && f->n.tx.quarantined);
	TEST_CHECK(f->n.control.quarantined && f->model.b.control_owned);
	retained = f->model.host.live_allocations;
	TEST_CHECK(retained > 6 && packet_frees == freed);
	TEST_CHECK(mana_netdev_shutdown(&f->n) == -ENOTCONN);
	TEST_CHECK(f->model.host.live_allocations == retained);
	TEST_CHECK(mana_bootstrap_stop(&f->model.b) == -EBUSY);
	/* The host has no DMA engine. Test disposal models external isolation. */
	TEST_CHECK(host_mock_cleanup(&f->model.host) == retained);
	uk_netbuf_free(pkt);
	return 0;
}

static int allocation_failures(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt;
	struct uk_netdev_rxqueue_conf rx = {
		.a = &allocator, .alloc_rxpkts = rx_allocate, .alloc_rxpkts_argp = f
	};
	struct uk_netdev_txqueue_conf tx = { .a = &allocator };
	unsigned int i, freed = packet_frees;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(PTR2ERR(f->n.dev.ops->rxq_configure(&f->n.dev, 0, 2, &rx)) ==
		   -EBUSY);
	TEST_CHECK(PTR2ERR(f->n.dev.ops->txq_configure(&f->n.dev, 1, 2, &tx)) ==
		   -EINVAL);
	/* EQ, both WQ/CQ pairs, and both receive buffers. */
	for (i = 1; i <= 7; i++) {
		TEST_CHECK(host_fault_arm(&f->model.host, HOST_FAULT_ALLOC, i,
					   -ENOMEM) == 0);
		TEST_CHECK(uk_netdev_start(&f->n.dev) == -ENOMEM);
		TEST_CHECK(!f->n.stopping && !f->n.data.control);
		TEST_CHECK(f->model.host.live_allocations == 6);
		TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	}
	TEST_CHECK(host_fault_arm(&f->model.host, HOST_FAULT_ALLOC, 0, -ENOMEM) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	pkt = packet(60);
	TEST_CHECK(pkt);
	TEST_CHECK(host_fault_arm(&f->model.host, HOST_FAULT_ALLOC, 1, -ENOMEM) == 0);
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, pkt) == -ENOMEM);
	TEST_CHECK(!f->n.tx.count && packet_frees == freed);
	uk_netbuf_free(pkt);
	return finish(f);
}

static int uncertain_rx_stop(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt;
	size_t retained;
	unsigned int freed = packet_frees;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	pkt = packet(60);
	TEST_CHECK(pkt && (uk_netdev_tx_one(&f->n.dev, 0, pkt) & UK_NETDEV_STATUS_SUCCESS));
	f->no_fence = 1;
	TEST_CHECK(mana_netdev_stop(&f->n) == -ETIMEDOUT);
	TEST_CHECK(f->n.rx.quarantined && f->n.tx.count == 1);
	TEST_CHECK(f->n.control.quarantined && f->model.b.control_owned);
	retained = f->model.host.live_allocations;
	TEST_CHECK(retained > 6 && packet_frees == freed && !f->destroys);
	TEST_CHECK(mana_netdev_shutdown(&f->n) == -ENOTCONN);
	TEST_CHECK(f->model.host.live_allocations == retained);
	TEST_CHECK(host_mock_cleanup(&f->model.host) == retained);
	uk_netbuf_free(pkt);
	return 0;
}

/* A fence CQE that rx_one finds outside teardown returns -EPROTO.
 * Teardown then reuses the validated fence and skips a second FENCE_RX.
 */
static int rx_fence_error(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *out = NULL;
	unsigned int destroys;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(mana_control_set_rx(&f->n.control, 0) == 0);
	TEST_CHECK(mana_control_fence_rx(&f->n.rx.queue.object) == 0);
	TEST_CHECK(f->n.rx.queue.object.fence_pending &&
		   !f->n.rx.queue.object.fenced);
	TEST_CHECK(uk_netdev_rx_one(&f->n.dev, 0, &out) == -EPROTO);
	TEST_CHECK(!out);
	TEST_CHECK(f->n.rx.queue.object.fenced &&
		   !f->n.rx.queue.object.fence_pending);
	destroys = f->destroys;
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(f->destroys == destroys + 2);
	return finish(f);
}

/* After a proven stop, the driver data-path callbacks reject with
 * -ESHUTDOWN and keep every packet reference with the caller.
 */
static int stopped_queues(void)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt, *out = NULL;
	unsigned int freed = packet_frees;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(mana_netdev_poll(&f->n) == -ESHUTDOWN);
	pkt = packet(60);
	TEST_CHECK(pkt);
	/* The pinned uknetdev API asserts the RUNNING state.
	 * The test calls the driver callback directly.
	 */
	TEST_CHECK(f->n.dev.tx_one(&f->n.dev, &f->n.txq, pkt) == -ESHUTDOWN);
	TEST_CHECK(f->n.dev.rx_one(&f->n.dev, &f->n.rxq, &out) == -ESHUTDOWN);
	TEST_CHECK(!out && !f->n.tx.count &&
		   uk_refcount_read(&pkt->refcount) == 1);
	uk_netbuf_free(pkt);
	TEST_CHECK(packet_frees == freed + 1);
	return finish(f);
}

/* The MORE flag follows the mana_tx_free_packets result for both the
 * 1-unit short WQE and the 2-unit long-form WQE. The queue model fills
 * the record FIFO, rejects one more packet, then frees space through
 * completions and refills.
 */
static int more_capacity(unsigned int short_form)
{
	struct fixture *f = fresh();
	struct uk_netbuf *pkt;
	uint32_t units = short_form ? 1U : 2U;
	unsigned int i, freed = packet_frees;
	int status;
	f->short_form = short_form;
	TEST_CHECK(register_device(f) == 0 &&
		   configure_device(f, MANA_NETDEV_DESCRIPTORS) == 0);
	TEST_CHECK(!mana_tx_free_packets(&f->n.tx));
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(f->n.tx.oob_size == (short_form ? 8U : 24U));
	TEST_CHECK(mana_tx_free_packets(&f->n.tx) == MANA_NETDEV_DESCRIPTORS);
	for (i = 0; i < MANA_NETDEV_DESCRIPTORS; i++) {
		pkt = packet(60);
		TEST_CHECK(pkt);
		status = uk_netdev_tx_one(&f->n.dev, 0, pkt);
		TEST_CHECK(status == (UK_NETDEV_STATUS_SUCCESS |
			   (mana_tx_free_packets(&f->n.tx) ?
			    UK_NETDEV_STATUS_MORE : 0)));
		TEST_CHECK(mana_tx_free_packets(&f->n.tx) ==
			   MANA_NETDEV_DESCRIPTORS - 1 - i);
		TEST_CHECK(f->n.tx.queue.head - f->n.tx.queue.tail ==
			   units * (i + 1));
	}
	pkt = packet(60);
	TEST_CHECK(pkt);
	TEST_CHECK(uk_netdev_tx_one(&f->n.dev, 0, pkt) == 0);
	TEST_CHECK(!mana_tx_free_packets(&f->n.tx) &&
		   uk_refcount_read(&pkt->refcount) == 1);
	for (i = 0; i < 4; i++)
		tx_completion(f, 32);
	status = uk_netdev_tx_one(&f->n.dev, 0, pkt);
	TEST_CHECK(status == (UK_NETDEV_STATUS_SUCCESS | UK_NETDEV_STATUS_MORE));
	TEST_CHECK(mana_tx_free_packets(&f->n.tx) == 3);
	for (i = 0; i < 3; i++) {
		pkt = packet(60);
		TEST_CHECK(pkt);
		status = uk_netdev_tx_one(&f->n.dev, 0, pkt);
		TEST_CHECK(status == (UK_NETDEV_STATUS_SUCCESS |
			   (i < 2 ? UK_NETDEV_STATUS_MORE : 0)));
	}
	TEST_CHECK(!mana_tx_free_packets(&f->n.tx));
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	TEST_CHECK(packet_frees == freed + 36 && !f->n.tx.count);
	TEST_CHECK(!mana_tx_free_packets(&f->n.tx));
	return finish(f);
}

static int more_short_wqe(void)
{
	return more_capacity(1);
}

static int more_long_wqe(void)
{
	return more_capacity(0);
}

/* Polling while the queues do not run returns -ESHUTDOWN and reports
 * no platform error event. Another rejected call still reports one.
 */
static int quiet_poll(void)
{
	struct fixture *f = fresh();
	struct uk_netdev_conf bad = { .nb_rx_queues = 2, .nb_tx_queues = 1 };
	unsigned int errors;
	TEST_CHECK(register_device(f) == 0 && configure_device(f, 2) == 0);
	TEST_CHECK(mana_netdev_poll(&f->n) == -ESHUTDOWN);
	TEST_CHECK(!f->model.errors);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	errors = f->model.errors;
	TEST_CHECK(mana_netdev_poll(&f->n) == 0);
	TEST_CHECK(f->n.dev.ops->configure(&f->n.dev, &bad) == -ENOTSUP);
	TEST_CHECK(f->model.errors == errors + 1);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	errors = f->model.errors;
	TEST_CHECK(mana_netdev_poll(&f->n) == -ESHUTDOWN);
	TEST_CHECK(mana_netdev_poll(&f->n) == -ESHUTDOWN);
	TEST_CHECK(f->model.errors == errors);
	TEST_CHECK(!mana_tx_free_packets(&f->n.tx));
	return finish(f);
}

/* A queue count above the reported maximum fails with -EINVAL and
 * leaves both queue owners unset. The maximum count then configures.
 */
static int queue_count_bounds(void)
{
	struct fixture *f = fresh();
	struct uk_netdev_conf conf = { .nb_rx_queues = 1, .nb_tx_queues = 1 };
	struct uk_netdev_txqueue_conf tx = { .a = &allocator };
	struct uk_netdev_rxqueue_conf rx = {
		.a = &allocator, .alloc_rxpkts = rx_allocate,
		.alloc_rxpkts_argp = f
	};
	TEST_CHECK(register_device(f) == 0);
	TEST_CHECK(uk_netdev_configure(&f->n.dev, &conf) == 0);
	TEST_CHECK(PTR2ERR(f->n.dev.ops->txq_configure(&f->n.dev, 0,
			MANA_NETDEV_DESCRIPTORS + 1, &tx)) == -EINVAL);
	TEST_CHECK(PTR2ERR(f->n.dev.ops->rxq_configure(&f->n.dev, 0,
			MANA_NETDEV_DESCRIPTORS + 1, &rx)) == -EINVAL);
	TEST_CHECK(!f->n.txq.owner && !f->n.rxq.owner);
	TEST_CHECK(uk_netdev_txq_configure(&f->n.dev, 0,
					   MANA_NETDEV_DESCRIPTORS, &tx) == 0);
	TEST_CHECK(uk_netdev_rxq_configure(&f->n.dev, 0,
					   MANA_NETDEV_DESCRIPTORS, &rx) == 0);
	TEST_CHECK(uk_netdev_start(&f->n.dev) == 0);
	TEST_CHECK(f->n.rx.count == MANA_NETDEV_DESCRIPTORS &&
		   f->n.tx.capacity == MANA_NETDEV_DESCRIPTORS);
	TEST_CHECK(mana_netdev_stop(&f->n) == 0);
	return finish(f);
}

int main(void)
{
	const struct host_test cases[] = {
		{ "default DMA gate prevents registration", dma_gate },
		{ "registration failure ownership", registration_errors },
		{ "real uknetdev lifecycle and capabilities", lifecycle },
		{ "TX acceptance completion and reference ownership", tx_ownership },
		{ "RX lengths allocator and refill ownership", rx_ownership },
		{ "partial start cleanup and retry", partial_start },
		{ "proven stop failure retains resources for retry", retry_stop },
		{ "uncertain TX drain retains cookies and DMA", uncertain_stop },
		{ "every startup allocation and TX rejection", allocation_failures },
		{ "missing RX fence blocks lower teardown", uncertain_rx_stop },
		{ "unexpected RX fence completion rejects with EPROTO", rx_fence_error },
		{ "stopped queues reject data-path calls with ESHUTDOWN", stopped_queues },
		{ "queue count above maximum leaves owners unset", queue_count_bounds },
		{ "MORE follows the TX capacity query with short WQEs", more_short_wqe },
		{ "MORE follows the TX capacity query with long WQEs", more_long_wqe },
		{ "poll while stopped returns ESHUTDOWN without error events", quiet_poll }
	};
	int result = host_test_run(cases, sizeof(cases) / sizeof(cases[0]));
	if (!result && buffer_allocs != packet_frees) {
		fprintf(stderr, "netbuf leak: %u allocations, %u frees\n",
			buffer_allocs, packet_frees);
		return 1;
	}
	return result;
}
