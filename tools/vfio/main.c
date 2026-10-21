/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MANA userspace VFIO runner. Each subcommand runs every gate up to and
 * including the requested one, so one live run reports the deepest gate
 * that passes on real firmware.
 *
 * Evidence contract (stdout, one event per line):
 *   GATE_PROBE|GATE_GDMA|GATE_CONTEXT|GATE_PACKET PASS|FAIL|SKIP
 *     [err=-N] [detail=key=value ...]
 *   IRQ_MODE=msix-eventfd|poll-fallback|none
 *   RUNNER_DONE <deepest stage or none>
 * Exit code: 0 when every requested gate passed or skipped, otherwise the
 * failed stage number (probe 1, gdma 2, context 3, packet 4). Usage errors
 * exit with 2 and print no RUNNER_DONE line.
 */

#define _GNU_SOURCE
#include "vfio-runner.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uk/mana.h>
#include <uk/mana/bootstrap.h>
#include <uk/mana/control.h>
#include <uk/mana/data_queue.h>
#include <uk/mana/rx.h>
#include <uk/mana/tx.h>

#define DEFAULT_TIMEOUT_MS 5000U

struct runner {
	struct vfio_ctx x;
	struct mana_bootstrap b;
	struct mana_control control;
	struct mana_data data;
	struct mana_tx tx;
	struct mana_tx_record records[8];
	struct mana_rx rx;
	struct mana_rx_slot slots[4];
	unsigned int deepest; /* deepest gate passed or skipped */
	char detail[512];
};

enum { STAGE_NONE = 0, STAGE_PROBE = 1, STAGE_GDMA = 2, STAGE_CONTEXT = 3,
       STAGE_PACKET = 4 };

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void emit(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void gate(struct runner *r, unsigned int id, const char *verdict,
		 int err)
{
	static const char *const names[] = { "-", "PROBE", "GDMA", "CONTEXT",
					     "PACKET" };

	printf("GATE_%s %s", names[id], verdict);
	if (err < 0)
		printf(" err=%d", err);
	if (r->detail[0])
		printf(" detail=%s", r->detail);
	printf("\n");
	fflush(stdout);
	if (strcmp(verdict, "FAIL") != 0 && id > r->deepest)
		r->deepest = id;
}

static void run_done(struct runner *r, int code)
{
	static const char *const names[] = { "none", "probe", "gdma", "context",
					     "packet" };

	printf("RUNNER_DONE %s\n", names[r->deepest]);
	fflush(stdout);
	vfio_close_device(&r->x);
	exit(code);
}

/* ---------- gate: probe ---------- */

static void dump_hex(const char *tag, const uint32_t *dwords, unsigned int n)
{
	printf("%s", tag);
	for (unsigned int i = 0; i < n; i++)
		printf(" %08x", dwords[i]);
	printf("\n");
}

static int stage_probe(struct runner *r, const char *bdf)
{
	uint32_t id, v, ptr = 0, msix_cap = 0, ctrl = 0;
	uint32_t row[4];
	char bars[384];
	unsigned int i, len, hops;
	int err;

	r->x.stage = "probe";
	r->detail[0] = 0;
	err = vfio_open_device(&r->x, bdf);
	if (err) {
		snprintf(r->detail, sizeof(r->detail), "step=vfio_open");
		gate(r, STAGE_PROBE, "FAIL", err);
		return err;
	}
	if (vfio_config_read(&r->x, 0, 4, &id)) {
		gate(r, STAGE_PROBE, "FAIL", -EIO);
		return -EIO;
	}
	if (id != (MANA_PCI_VENDOR_ID | MANA_PCI_VF_DEVICE_ID << 16)) {
		emit("ERROR device at %s reports id=0x%08x, expected 0x00ba1414."
		     " The chosen vfio group holds another device?", bdf, id);
		gate(r, STAGE_PROBE, "FAIL", -ENODEV);
		return -ENODEV;
	}
	len = 0;
	bars[0] = '\0';
	(void)vfio_config_read(&r->x, 0x34, 1, &ptr);
	/* The 48 cap bound also stops a malformed capability cycle. */
	for (hops = 0; hops < 48 && ptr >= 0x40 && ptr <= 0xfc; hops++) {
		if (vfio_config_read(&r->x, (uint16_t)ptr, 4, &v))
			break;
		if (len < sizeof(bars) - 1)
			len += (unsigned int)snprintf(
				bars + len, sizeof(bars) - len, "%s0x%x@0x%x",
				len ? "," : "", v & 0xff, ptr);
		if ((v & 0xff) == 0x11)
			msix_cap = ptr;
		ptr = (v >> 8) & 0xff;
	}
	if (!msix_cap) {
		emit("ERROR no MSI-X capability. The driver requires at least"
		     " one MSI-X vector.");
		gate(r, STAGE_PROBE, "FAIL", -ENOTSUP);
		return -ENOTSUP;
	}
	(void)vfio_config_read(&r->x, (uint16_t)(msix_cap + 2), 2, &ctrl);
	(void)vfio_config_read(&r->x, (uint16_t)(msix_cap + 4), 4,
			       &r->x.msix_table);
	(void)vfio_config_read(&r->x, (uint16_t)(msix_cap + 8), 4, &r->x.msix_pba);
	if (r->x.msix_vectors == 0)
		r->x.msix_vectors = (ctrl & 0x7ff) + 1;
	emit("CAPS %s", bars);
	emit("MSIX vectors=%u control=0x%x table=0x%x pba=0x%x",
	     r->x.msix_vectors, ctrl, r->x.msix_table, r->x.msix_pba);
	(void)vfio_irq_probe(&r->x);
	if (r->x.irq_mode == VFIO_IRQ_MODE_MSIX_EVENTFD) {
		emit("IRQ_MODE=msix-eventfd");
	} else if (r->x.irq_errno) {
		emit("IRQ_MODE=poll-fallback set_irqs_err=%d", r->x.irq_errno);
		emit("NOTE kernel refused MSI-X SET_IRQS. The runner counts a"
		     " delivery from its poll loop instead. Real interrupt"
		     " delivery is NOT validated in this mode.");
		r->x.irq_mode = VFIO_IRQ_MODE_POLL_FALLBACK;
	} else {
		emit("IRQ_MODE=none vectors=%u", r->x.msix_vectors);
		emit("NOTE no usable MSI-X vector. Gates that need interrupt"
		     " delivery will fail, not silently pass.");
	}
	for (i = 0; i < 4; i++)
		(void)vfio_config_read(&r->x, (uint16_t)(i * 4), 4, &row[i]);
	dump_hex("CONFIG00:", row, 4);
	for (i = 0; i < 4; i++)
		(void)vfio_config_read(&r->x, (uint16_t)(0x10 + i * 4), 4,
				       &row[i]);
	dump_hex("CONFIG10:", row, 4);
	len = 0;
	bars[0] = '\0';
	for (i = 0; i < VFIO_BAR_COUNT; i++) {
		if (!r->x.bars[i].present)
			continue;
		if (len < sizeof(bars) - 1)
			len += (unsigned int)snprintf(
				bars + len, sizeof(bars) - len, "%sbar%u:0x%"
				PRIx64 "+0x%" PRIx64, len ? "," : "", i,
				r->x.bars[i].base, r->x.bars[i].size);
	}
	emit("BARS %s", bars);
	if (!r->x.bars[0].present || r->x.bars[0].size < 0x20) {
		gate(r, STAGE_PROBE, "FAIL", -ENODEV);
		return -ENODEV;
	}
	{
		uint32_t db_lo = vfio_bar_read32(&r->x, 0, 8);
		uint32_t db_hi = vfio_bar_read32(&r->x, 0, 12);
		uint32_t db_size = vfio_bar_read32(&r->x, 0, 0x10) & 0xffff;
		uint32_t shm_lo = vfio_bar_read32(&r->x, 0, 0x18);
		uint32_t shm_hi = vfio_bar_read32(&r->x, 0, 0x1c);
		uint64_t db = (uint64_t)db_lo | (uint64_t)db_hi << 32;
		uint64_t shm = (uint64_t)shm_lo | (uint64_t)shm_hi << 32;
		uint32_t status = UINT32_MAX;
		int usable_range = (shm & 3) == 0 && shm >= 0x20 &&
				   shm + 32 <= r->x.bars[0].size;

		if (usable_range)
			status = vfio_bar_read32(&r->x, 0, shm + 28);
		emit("GDMA db=0x%" PRIx64 " db_size=0x%x shm=0x%" PRIx64
		     " smc_status=0x%08x", db, db_size, shm, status);
		if (usable_range && (status & 0x80000000U))
			emit("NOTE SMC status busy bit set at probe time. The"
			     " driver will wait for it later.");
		snprintf(r->detail, sizeof(r->detail),
			 "id=0x%08x bars=%s msix=%u shm=0x%" PRIx64
			 " smc_status=0x%08x", id, bars, r->x.msix_vectors, shm,
			 status);
	}
	gate(r, STAGE_PROBE, "PASS", 0);
	return 0;
}

/* ---------- gate: gdma ---------- */

static int stage_gdma(struct runner *r)
{
	int err;

	r->x.stage = "gdma";
	r->detail[0] = 0;
	vfio_begin_wait(&r->x, "bootstrap(SMC+HWC-init+GDMA-query)");
	err = mana_bootstrap_start(&r->b, vfio_platform(&r->x));
	if (err) {
		snprintf(r->detail, sizeof(r->detail),
			 "init_seen=0x%x dma_exposed=%u quarantined=%u"
			 " irq_events=%llu",
			 r->b.init_seen, r->b.dma_exposed, r->b.quarantined,
			 (unsigned long long)r->x.irq_events);
		gate(r, STAGE_GDMA, "FAIL", err);
		return err;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "eq=%u cq=%u rq=%u sq=%u db=%u pd=%u mkey=%u max_req=%u"
		 " max_resp=%u max_cqs=%u depth=%u dev_id=%u lim(sq,rq,cq,eq,db,"
		 "msix)=%u,%u,%u,%u,%u,%u irq_events=%llu",
		 r->b.eq_id, r->b.cq_id, r->b.rq_id, r->b.sq_id, r->b.db_id,
		 r->b.pd_id, r->b.mkey, r->b.max_req, r->b.max_resp,
		 r->b.max_cqs, r->b.depth, r->b.mana_device_id, r->b.limits.sq,
		 r->b.limits.rq, r->b.limits.cq, r->b.limits.eq,
		 r->b.limits.doorbells, r->b.limits.msix,
		 (unsigned long long)r->x.irq_events);
	gate(r, STAGE_GDMA, "PASS", 0);
	return 0;
}

/* ---------- gate: context ---------- */

static int stage_context(struct runner *r)
{
	int err;

	r->x.stage = "context";
	r->detail[0] = 0;
	vfio_begin_wait(&r->x, "control(register+query+config)");
	err = mana_control_start(&r->control, &r->b, 0);
	if (err) {
		snprintf(r->detail, sizeof(r->detail),
			 "registered=%u configured=%u quarantined=%u",
			 r->control.registered, r->control.configured,
			 r->control.quarantined);
		gate(r, STAGE_CONTEXT, "FAIL", err);
		return err;
	}
	snprintf(r->detail, sizeof(r->detail),
		 "pd=%u mkey=%u doorbell=%u ports=%u mtu=%u mac=%02x:%02x:%02x:"
		 "%02x:%02x:%02x vport=0x%" PRIx64 " link=%u qos=%u link_err=%d"
		 " short_form=%u bm_hostmode=%u",
		 r->control.pd_id, r->control.mkey, r->control.doorbell_id,
		 r->control.max_ports, r->control.mtu, r->control.mac[0],
		 r->control.mac[1], r->control.mac[2], r->control.mac[3],
		 r->control.mac[4], r->control.mac[5], r->control.vport,
		 r->control.link_speed_mbps, r->control.qos_speed_mbps,
		 r->control.link_query_error, r->control.short_form_allowed,
		 r->control.bm_hostmode);
	gate(r, STAGE_CONTEXT, "PASS", 0);
	return 0;
}

/* ---------- gate: packet ---------- */

static void build_arp(struct runner *r, unsigned char *frame)
{
	memset(frame, 0, 42);
	memset(frame, 0xff, 6); /* broadcast destination */
	memcpy(frame + 6, r->control.mac, 6);
	frame[12] = 0x08;
	frame[13] = 0x06; /* ARP ethertype */
	frame[14] = 0;
	frame[15] = 1; /* hardware: Ethernet */
	frame[16] = 0x08;
	frame[17] = 0; /* protocol: IPv4 */
	frame[18] = 6;
	frame[19] = 4;
	frame[20] = 0;
	frame[21] = 1; /* request */
	memcpy(frame + 22, r->control.mac, 6);
	frame[32] = 10;
	frame[33] = 0;
	frame[34] = 0;
	frame[35] = 1; /* sender IP, arbitrary */
	frame[38] = 10;
	frame[39] = 0;
	frame[40] = 0;
	frame[41] = 2; /* target IP, arbitrary */
}

static void drain_cb(void *ctx, const struct mana_tx_result *result)
{
	printf("DRAIN cookie=%p error=%d\n", result->cookie, result->error);
	fflush(stdout);
	(void)ctx;
}

/* A firmware rejection with a clean state is a skip, not a driver fault. */
static int rejected(int err, unsigned int quarantined)
{
	if (quarantined)
		return 0;
	return err == -EOPNOTSUPP || err == -ENOTSUP || err == -EPROTO ||
	       err == -ENOSPC;
}

static int stage_packet(struct runner *r)
{
	unsigned char frame[42];
	struct mana_tx_result result = { 0 };
	struct mana_rx_packet packet = { 0 };
	uint64_t stop_ms;
	int err, tx_done = 0, rx_seen = 0, rx_dropped = 0;

	r->x.stage = "packet";
	r->detail[0] = 0;
	vfio_begin_wait(&r->x, "dataEQ(create+self-test)");
	err = mana_data_start(&r->data, &r->control, 4096);
	if (err) {
		snprintf(r->detail, sizeof(r->detail), "step=data_start");
		gate(r, STAGE_PACKET,
		     rejected(err, r->data.quarantined ||
					  r->control.quarantined) ? "SKIP"
								  : "FAIL",
		     err);
		return err;
	}
	vfio_begin_wait(&r->x, "TX(create)");
	err = mana_tx_start(&r->tx, &r->data, r->records, 8, 4096, 4096);
	if (!err) {
		vfio_begin_wait(&r->x, "RX(create+refill+steer)");
		err = mana_rx_start(&r->rx, &r->data, r->slots, 4, 4096, 4096);
	}
	if (err) {
		snprintf(r->detail, sizeof(r->detail),
			 "step=queue_create tx=%u rx=%u", r->tx.ready,
			 r->rx.ready);
		gate(r, STAGE_PACKET,
		     rejected(err, r->data.quarantined) ? "SKIP" : "FAIL", err);
		return err;
	}
	build_arp(r, frame);
	err = mana_tx_submit(&r->tx, frame, sizeof(frame), (void *)0x1);
	if (err != 1) {
		snprintf(r->detail, sizeof(r->detail), "step=tx_submit ret=%d",
			 err);
		gate(r, STAGE_PACKET, "FAIL", err < 0 ? err : -EPROTO);
		return err < 0 ? err : -EPROTO;
	}
	stop_ms = now_ms() + r->x.timeout_ms;
	for (;;) {
		vfio_pump_irq(&r->x);
		err = mana_tx_poll(&r->tx, &result);
		if (err < 0) {
			snprintf(r->detail, sizeof(r->detail), "step=tx_poll");
			gate(r, STAGE_PACKET, "FAIL", err);
			return err;
		}
		if (err == 1) {
			tx_done = 1;
			break;
		}
		if (now_ms() >= stop_ms)
			break;
		usleep(1000);
	}
	if (!tx_done) {
		emit("WAIT_TIMEOUT waited=%ums for=tx-completion"
		     " env=MANA_VFIO_TIMEOUT_MS=%u", r->x.timeout_ms,
		     r->x.timeout_ms);
		snprintf(r->detail, sizeof(r->detail), "step=tx_completion");
		gate(r, STAGE_PACKET, "FAIL", -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	stop_ms = now_ms() + 200; /* one best-effort look for any RX traffic */
	for (;;) {
		vfio_pump_irq(&r->x);
		err = mana_rx_poll(&r->rx, &packet);
		if (err == MANA_RX_PACKET) {
			rx_seen = 1;
			printf("RX_PACKET length=%u first_byte=0x%02x\n",
			       packet.length,
			       ((unsigned char *)packet.dma.addr)[0]);
			fflush(stdout);
			(void)mana_rx_packet_release(&packet);
			break;
		}
		if (err == MANA_RX_DROPPED) {
			rx_dropped++;
			err = mana_rx_refill(&r->rx);
			if (err < 0) {
				snprintf(r->detail, sizeof(r->detail),
					 "step=rx_refill");
				gate(r, STAGE_PACKET, "FAIL", err);
				return err;
			}
		}
		if (err < 0) {
			snprintf(r->detail, sizeof(r->detail), "step=rx_poll");
			gate(r, STAGE_PACKET, "FAIL", err);
			return err;
		}
		if (now_ms() >= stop_ms)
			break;
		usleep(1000);
	}
	snprintf(r->detail, sizeof(r->detail),
		 "tx=complete tx_err=%d type=%u rx_seen=%d rx_dropped=%d"
		 " irq_events=%llu",
		 result.error, result.type, rx_seen, rx_dropped,
		 (unsigned long long)r->x.irq_events);
	gate(r, STAGE_PACKET, "PASS", 0);
	return 0;
}

/* ---------- teardown ---------- */

static void note_stop(const char *what, int err)
{
	if (!err) {
		printf("TEARDOWN %s ok\n", what);
		return;
	}
	printf("TEARDOWN %s err=%d note=ownership-retained; rebind vfio-pci to"
	       " reset the VF\n", what, err);
	fflush(stdout);
}

static void teardown(struct runner *r)
{
	vfio_begin_wait(&r->x, "teardown");
	if (r->rx.queue.data)
		note_stop("rx_stop", mana_rx_stop(&r->rx));
	if (r->tx.queue.data)
		note_stop("tx_stop", mana_tx_stop(&r->tx, drain_cb, NULL));
	if (r->data.control)
		note_stop("data_stop", mana_data_stop(&r->data));
	if (r->control.bootstrap)
		note_stop("control_stop", mana_control_stop(&r->control));
	/*
	 * Stop the bootstrap when a gate passed, or when a failed start left
	 * DMA exposed or quarantined. A failed start whose internal stop
	 * already succeeded owns nothing, and a second stop would double
	 * unmap BAR zero, so skip that case.
	 */
	if (r->b.p && (r->deepest >= STAGE_GDMA || r->b.dma_exposed ||
		       r->b.quarantined))
		note_stop("bootstrap_stop", mana_bootstrap_stop(&r->b));
	if (r->x.dma_allocs)
		emit("NOTE %llu DMA allocations remained (quarantine); process"
		     " exit frees the memory",
		     (unsigned long long)r->x.dma_allocs);
}

/* ---------- CLI ---------- */

static void usage(void)
{
	printf("Usage: mana-vfio-runner <gate> <bdf>\n"
	       "  gate: probe | gdma | context | packet | selftest\n"
	       "  bdf:  [domain:]bus:device.function, for example 0000:78:00.0\n"
	       "        Find the MANA VF with: lspci -Dnd 1414:00ba\n"
	       "The device must already be bound to vfio-pci. This tool never\n"
	       "binds or unbinds drivers.\n"
	       "Environment:\n"
	       "  MANA_VFIO_TIMEOUT_MS  wait budget per protocol step (default\n"
	       "                        %u)\n"
	       "selftest needs no device. It proves the DMA path and the\n"
	       "pagemap PFN contiguity check on this machine.\n",
	       DEFAULT_TIMEOUT_MS);
	fflush(stdout);
}

static int normalize_bdf(const char *in, char *out, size_t cap)
{
	unsigned int dom = 0, bus = 0, dev = 0, func = 0;

	if (sscanf(in, "%4x:%2x:%2x.%1x", &dom, &bus, &dev, &func) == 4) {
		snprintf(out, cap, "%04x:%02x:%02x.%x", dom, bus, dev, func);
		return 0;
	}
	if (sscanf(in, "%2x:%2x.%1x", &bus, &dev, &func) == 3) {
		snprintf(out, cap, "0000:%02x:%02x.%x", bus, dev, func);
		return 0;
	}
	return -EINVAL;
}

int main(int argc, char **argv)
{
	static struct runner r; /* zeroed: the driver requires fresh storage */
	char bdf[32];
	const char *env;
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0); /* evidence lines reach pipes now */
	if ((argc != 2 && argc != 3) ||
	    (strcmp(argv[1], "selftest") != 0 && argc != 3)) {
		usage();
		return 2;
	}
	if (strcmp(argv[1], "selftest") == 0)
		return vfio_selftest();
	if (strcmp(argv[1], "probe") != 0 && strcmp(argv[1], "gdma") != 0 &&
	    strcmp(argv[1], "context") != 0 && strcmp(argv[1], "packet") != 0) {
		printf("ERROR unknown gate '%s'.\n", argv[1]);
		usage();
		return 2;
	}
	if (normalize_bdf(argv[2], bdf, sizeof(bdf)) != 0) {
		printf("ERROR invalid BDF '%s'. Expected [domain:]bus:device."
		       "function.\n", argv[2]);
		return 2;
	}
	env = getenv("MANA_VFIO_TIMEOUT_MS");
	r.x.timeout_ms = env ? (unsigned int)strtoul(env, NULL, 10)
			     : DEFAULT_TIMEOUT_MS;
	if (r.x.timeout_ms < 100 || r.x.timeout_ms > 1200000) {
		printf("ERROR MANA_VFIO_TIMEOUT_MS=%u is out of the range 100"
		       " to 1200000.\n", r.x.timeout_ms);
		return 2;
	}
	r.x.container_fd = -1;
	r.x.group_fd = -1;
	r.x.device_fd = -1;
	r.x.event_fd = -1;

	err = stage_probe(&r, bdf);
	if (err)
		run_done(&r, VFIO_EXIT_PROBE);
	if (strcmp(argv[1], "probe") == 0)
		run_done(&r, VFIO_EXIT_OK);

	err = stage_gdma(&r);
	if (err) {
		teardown(&r);
		run_done(&r, VFIO_EXIT_GDMA);
	}
	if (strcmp(argv[1], "gdma") == 0) {
		teardown(&r);
		run_done(&r, VFIO_EXIT_OK);
	}
	err = stage_context(&r);
	if (err) {
		teardown(&r);
		run_done(&r, VFIO_EXIT_CONTEXT);
	}
	if (strcmp(argv[1], "context") == 0) {
		teardown(&r);
		run_done(&r, VFIO_EXIT_OK);
	}
	err = stage_packet(&r);
	teardown(&r);
	if (err && r.deepest < STAGE_PACKET)
		run_done(&r, VFIO_EXIT_PACKET);
	run_done(&r, VFIO_EXIT_OK);
	return 0;
}
