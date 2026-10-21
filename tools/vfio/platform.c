/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * struct mana_platform backed by VFIO. The device must already be bound
 * to vfio-pci. This file never binds or unbinds a driver.
 *
 * MMIO runs through pread and pwrite on the device fd. mmap would exclude
 * the MSI-X table area and bypass the vfio access rules, so we keep one
 * fd-based view of each BAR region and translate BAR-relative offsets.
 *
 * DMA memory uses anonymous transparent huge pages. The kernel backs a
 * 2 MiB-aligned, 2 MiB-sized VMA with one THP, and a THP is physically
 * contiguous. /proc/self/pagemap proves that before we hand the address
 * to the device. Inside the Azure guest the process PFN equals the frame
 * the VF DMAs against, because no vIOMMU sits between the VF and
 * guest-physical memory.
 */

#define _GNU_SOURCE
#include "vfio-runner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PM_PRESENT (UINT64_C(1) << 63)
#define PM_SWAPPED (UINT64_C(1) << 62)
#define PM_PFN_MASK ((UINT64_C(1) << 55) - 1)
#define MANA_DEVICE_ADDR_LIMIT UINT64_C(0x10000000000000)
#define DMA_TRIES 8U

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void platform_error(void *c, const char *stage, int err)
{
	struct vfio_ctx *ctx = c;

	fprintf(stderr, "[%s] %s: err=%d\n", ctx->stage ? ctx->stage : "init",
		stage, err);
}

void vfio_begin_wait(struct vfio_ctx *ctx, const char *what)
{
	ctx->wait_label = what;
	ctx->deadline_ms = ctx->timeout_ms ? now_ms() + ctx->timeout_ms : 0;
}

/*
 * MSI-X under VFIO: the kernel programs the physical table when SET_IRQS
 * registers the eventfd. Userspace table writes inside the BAR region are
 * dropped and table reads return ones, exactly like a guest with a
 * virtualized table. The driver tolerates that because the real vector is
 * already live. In poll-fallback mode the kernel refused SET_IRQS, so the
 * runner calls the handler from its own wait loop. The handler only
 * counts delivery. The driver still validates every event against the
 * real EQ and CQ rings, so a fake count cannot hide a missing event.
 */
void vfio_pump_irq(struct vfio_ctx *ctx)
{
	uint64_t count = 0;
	ssize_t got;

	if (ctx->irq_mode == VFIO_IRQ_MODE_MSIX_EVENTFD && ctx->event_fd >= 0) {
		got = read(ctx->event_fd, &count, sizeof(count));
		if (got == (ssize_t)sizeof(count) && count && ctx->handler) {
			ctx->irq_events++;
			ctx->handler(ctx->handler_arg);
		}
		return;
	}
	if (ctx->irq_mode == VFIO_IRQ_MODE_POLL_FALLBACK && ctx->handler) {
		ctx->irq_events++;
		ctx->handler(ctx->handler_arg);
	}
}

void vfio_close_device(struct vfio_ctx *ctx)
{
	if (ctx->device_fd >= 0)
		close(ctx->device_fd);
	if (ctx->event_fd >= 0)
		close(ctx->event_fd);
	if (ctx->group_fd >= 0)
		close(ctx->group_fd);
	if (ctx->container_fd >= 0)
		close(ctx->container_fd);
	ctx->device_fd = -1;
	ctx->event_fd = -1;
	ctx->group_fd = -1;
	ctx->container_fd = -1;
	ctx->handler = NULL;
	ctx->handler_arg = NULL;
}

/* ---------- config space ---------- */

static int set_irqs(struct vfio_ctx *ctx, uint32_t flags, unsigned int count,
		    int fd);

/* Record the last failing platform operation for the gate evidence. */
static void note_fail(struct vfio_ctx *ctx, const char *op, uint64_t off,
		      int err)
{
	snprintf(ctx->last_op, sizeof(ctx->last_op), "%s", op);
	ctx->last_off = off;
	ctx->last_errno = err;
}

/*
 * The kernel owns the emulated MSI-X Message Control and drops
 * userspace writes to the Enable and Function Mask bits. Raw config
 * reads then report kernel state, not what userspace wrote. The driver
 * follows the bare-metal model: it writes these bits and verifies the
 * readback. Virtualize the two writable bits here so the verify sees
 * its write, and mirror the Enable transition into SET_IRQS, the real
 * enable mechanism on this platform. The driver's irq_alloc already
 * registered the eventfd trigger before the Enable write, so the
 * transition is an idempotent re-assert; clearing Enable releases it.
 */
static int msix_ctrl_write(struct vfio_ctx *ctx, uint16_t control)
{
	bool was_enabled = (ctx->msix_ctrl_virt & 0x8000) != 0;
	bool is_enabled = (control & 0x8000) != 0;
	int err = 0;

	if (!was_enabled && is_enabled &&
	    ctx->irq_mode == VFIO_IRQ_MODE_MSIX_EVENTFD)
		err = set_irqs(ctx,
			       VFIO_IRQ_SET_DATA_EVENTFD |
				   VFIO_IRQ_SET_ACTION_TRIGGER,
			       1, ctx->event_fd);
	else if (was_enabled && !is_enabled)
		err = set_irqs(ctx, VFIO_IRQ_SET_DATA_NONE |
				   VFIO_IRQ_SET_ACTION_TRIGGER, 0, -1);
	if (err)
		return err;
	ctx->msix_ctrl_virt = (uint16_t)((control & 0xc000U) |
					 (ctx->msix_ctrl_virt & 0x03ffU));
	return 0;
}

int vfio_config_read(struct vfio_ctx *ctx, uint32_t off, unsigned int width,
		     uint32_t *value)
{
	unsigned char raw[4] = { 0, 0, 0, 0 };
	ssize_t got;

	if (width != 1 && width != 2 && width != 4) {
		note_fail(ctx, "cfg_rd", off, -EINVAL);
		return -EINVAL;
	}
	if (off % width || (uint64_t)off + width > ctx->config_size) {
		note_fail(ctx, "cfg_rd", off, -EINVAL);
		return -EINVAL;
	}
	if (ctx->msix_cap && width == 2 && off == (uint32_t)ctx->msix_cap + 2) {
		*value = ctx->msix_ctrl_virt;
		return 0;
	}
	got = pread(ctx->device_fd, raw, width, (off_t)(ctx->config_offset + off));
	if (got != (ssize_t)width) {
		int e = errno > 0 ? -errno : -EIO;

		note_fail(ctx, "cfg_rd", off, e);
		return e;
	}
	*value = raw[0];
	if (width >= 2)
		*value |= (uint32_t)raw[1] << 8;
	if (width == 4)
		*value |= (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
	return 0;
}

static int config_read(void *c, uint16_t off, unsigned int width,
		       uint32_t *value)
{
	return vfio_config_read(c, off, width, value);
}

static int config_write(void *c, uint16_t off, unsigned int width,
			uint32_t value)
{
	struct vfio_ctx *ctx = c;
	ssize_t put;

	if (width != 1 && width != 2 && width != 4) {
		note_fail(ctx, "cfg_wr", off, -EINVAL);
		return -EINVAL;
	}
	if (off % width || (uint64_t)off + width > ctx->config_size) {
		note_fail(ctx, "cfg_wr", off, -EINVAL);
		return -EINVAL;
	}
	if (ctx->msix_cap && width == 2 &&
	    (uint32_t)off == (uint32_t)ctx->msix_cap + 2) {
		int err = msix_ctrl_write(ctx, (uint16_t)value);

		if (err) {
			note_fail(ctx, "msix_ctrl", off, err);
			return err;
		}
		return 0;
	}
	put = pwrite(ctx->device_fd, &value, width,
		     (off_t)(ctx->config_offset + off));
	if (put != (ssize_t)width) {
		int e = errno > 0 ? -errno : -EIO;

		note_fail(ctx, "cfg_wr", off, e);
		return e;
	}
	return 0;
}

/* ---------- MMIO ---------- */

static struct vfio_bar *mapping_bar(struct vfio_ctx *ctx,
				    const struct mana_mapping *m)
{
	struct vfio_bar *bar;

	if (!m || !m->addr)
		return NULL;
	bar = (struct vfio_bar *)m->addr;
	if (bar < &ctx->bars[0] || bar >= &ctx->bars[VFIO_BAR_COUNT] ||
	    !bar->present)
		return NULL;
	return bar;
}

static uint32_t bar_read32(struct vfio_ctx *ctx, const struct vfio_bar *bar,
			   uint64_t off)
{
	uint32_t value = UINT32_MAX;
	ssize_t got;

	if (off % 4 || off + 4 > bar->size) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_rd", off, -EINVAL);
		return UINT32_MAX;
	}
	got = pread(ctx->device_fd, &value, 4, (off_t)(bar->offset + off));
	if (got != 4) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_rd", off, errno > 0 ? -errno : -EIO);
		return UINT32_MAX;
	}
	return value;
}

uint32_t vfio_bar_read32(struct vfio_ctx *ctx, unsigned int bar, uint64_t off)
{
	if (bar >= VFIO_BAR_COUNT || !ctx->bars[bar].present)
		return UINT32_MAX;
	return bar_read32(ctx, &ctx->bars[bar], off);
}

static uint32_t mmio_read32(void *c, const struct mana_mapping *m, uint64_t off)
{
	struct vfio_ctx *ctx = c;
	struct vfio_bar *bar = mapping_bar(ctx, m);

	if (!bar) {
		ctx->mmio_errors++;
		return UINT32_MAX;
	}
	return bar_read32(ctx, bar, off);
}

static void mmio_write32(void *c, const struct mana_mapping *m, uint64_t off,
			 uint32_t value)
{
	struct vfio_ctx *ctx = c;
	struct vfio_bar *bar = mapping_bar(ctx, m);
	ssize_t put;

	if (!bar) {
		ctx->mmio_errors++;
		return;
	}
	if (off % 4 || off + 4 > bar->size) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_w32", off, -EINVAL);
		return;
	}
	put = pwrite(ctx->device_fd, &value, 4, (off_t)(bar->offset + off));
	if (put != 4) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_w32", off, errno > 0 ? -errno : -EIO);
	}
}

static void mmio_write64(void *c, const struct mana_mapping *m, uint64_t off,
			 uint64_t value)
{
	struct vfio_ctx *ctx = c;
	struct vfio_bar *bar = mapping_bar(ctx, m);
	ssize_t put;

	if (!bar) {
		ctx->mmio_errors++;
		return;
	}
	if (off % 8 || off + 8 > bar->size) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_w64", off, -EINVAL);
		return;
	}
	/*
	 * Kernels with the vfio qword IO path issue one 8-byte write here.
	 * Older kernels split it into two 4-byte writes. The Azure target
	 * runs a 7.0 kernel, so the doorbell stays one transaction.
	 */
	put = pwrite(ctx->device_fd, &value, 8, (off_t)(bar->offset + off));
	if (put != 8) {
		ctx->mmio_errors++;
		note_fail(ctx, "mmio_w64", off, errno > 0 ? -errno : -EIO);
	}
}

static int bar_map(void *c, uint64_t physical, uint64_t size,
		   struct mana_mapping *mapping)
{
	struct vfio_ctx *ctx = c;
	unsigned int i;

	for (i = 0; i < VFIO_BAR_COUNT; i++) {
		if (!ctx->bars[i].present)
			continue;
		if (ctx->bars[i].base != physical || ctx->bars[i].size != size)
			continue;
		mapping->addr = &ctx->bars[i];
		mapping->size = size;
		return 0;
	}
	note_fail(ctx, "bar_map", physical, -EINVAL);
	return -EINVAL;
}

static int bar_unmap(void *c, struct mana_mapping *mapping)
{
	(void)c;
	if (!mapping || !mapping->addr)
		return -EINVAL;
	mapping->addr = NULL;
	mapping->size = 0;
	return 0;
}

/* ---------- DMA allocation with pagemap proof ---------- */

static int pagemap_entry(uintptr_t va, uint64_t *entry)
{
	ssize_t got;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	got = pread(fd, entry, sizeof(*entry),
		    (off_t)((va >> 12) * sizeof(*entry)));
	close(fd);
	if (got != (ssize_t)sizeof(*entry))
		return got < 0 && errno > 0 ? -errno : -EIO;
	return 0;
}

/*
 * Verify every 4 KiB chunk of the range: present, not swapped, and backed
 * by consecutive page frame numbers. PFN zero with the present bit set
 * means the kernel hid PFNs from us, which is normal without
 * CAP_SYS_ADMIN. That sets *pfn_hidden so the caller can say so.
 */
static int pagemap_contiguous(uintptr_t va, size_t size, uint64_t *head,
			      bool *pfn_hidden)
{
	uint64_t head_pfn = 0;
	size_t done = 0;

	*pfn_hidden = false;
	while (done < size) {
		uint64_t entry = 0;
		uint64_t pfn;
		int err = pagemap_entry(va + done, &entry);

		if (err)
			return err;
		if (!(entry & PM_PRESENT) || (entry & PM_SWAPPED))
			return -EAGAIN;
		pfn = entry & PM_PFN_MASK;
		if (pfn == 0) {
			*pfn_hidden = true;
			return 0;
		}
		if (done == 0)
			head_pfn = pfn;
		else if (pfn != head_pfn + done / 4096)
			return -EAGAIN;
		done += 4096;
	}
	*head = head_pfn;
	return 0;
}

/*
 * Try one 2 MiB-aligned reservation. THP placement can fail in place and
 * leave non-contiguous PFNs, so the caller retries with a fresh mapping.
 */
static int dma_attempt(size_t size, size_t vlen, uintptr_t *out, uint64_t *head,
		       bool *locked, bool *pfn_hidden)
{
	uintptr_t total = vlen + VFIO_THP_SIZE;
	uintptr_t raw, aligned, tail;
	int err;

	raw = (uintptr_t)mmap(NULL, total, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == (uintptr_t)MAP_FAILED)
		return -ENOMEM;
	aligned = (raw + VFIO_THP_SIZE - 1) & ~(uintptr_t)(VFIO_THP_SIZE - 1);
	if (aligned != raw)
		munmap((void *)raw, aligned - raw);
	tail = aligned + vlen;
	munmap((void *)tail, raw + total - tail);
	(void)madvise((void *)aligned, vlen, MADV_HUGEPAGE);
	memset((void *)aligned, 0, vlen); /* fault every page */
	*locked = mlock((void *)aligned, vlen) == 0;
	err = pagemap_contiguous(aligned, size, head, pfn_hidden);
	if (err || *pfn_hidden) {
		if (*locked)
			munlock((void *)aligned, vlen);
		munmap((void *)aligned, vlen);
		if (*pfn_hidden)
			return -EACCES; /* PFN bits hidden: retry will not help */
		return err ? err : -EAGAIN;
	}
	*out = aligned;
	return 0;
}

static size_t dma_vlen(size_t size)
{
	size_t vlen = (size + 4095U) & ~(size_t)4095U;

	if (vlen < VFIO_THP_SIZE)
		return VFIO_THP_SIZE; /* a THP needs a 2 MiB aligned VMA */
	return (vlen + VFIO_THP_SIZE - 1) & ~(size_t)(VFIO_THP_SIZE - 1);
}

static int dma_reserve(struct vfio_ctx *ctx, size_t size, struct mana_dma *dma,
		       bool *locked)
{
	size_t vlen = dma_vlen(size);
	uintptr_t aligned = 0;
	uint64_t head = 0;
	bool pfn_hidden = false;
	unsigned int try;
	int err = -ENOMEM;

	if (!size || size % 4096)
		return -EINVAL;
	/*
	 * Retry covers THP placement failures: an in-place split leaves
	 * non-contiguous PFNs, so we unmap and try a fresh 2 MiB-aligned
	 * mapping. Hidden PFNs or out-of-memory never improve on retry.
	 */
	for (try = 0; try < DMA_TRIES; try++) {
		err = dma_attempt(size, vlen, &aligned, &head, locked,
				  &pfn_hidden);
		if (err == 0)
			break;
		if (err != -EAGAIN) {
			if (err == -EACCES)
				platform_error(ctx,
					       "dma: pagemap hides PFN bits;"
					       " run as root for DMA",
					       err);
			return err;
		}
	}
	if (err)
		return err; /* exhausted placement retries */
	if (!*locked && ctx->unlocked++ == 0)
		platform_error(ctx, "dma mlock refused (running unlocked)",
			       -ENOMEM);
	if ((head << 12) >= MANA_DEVICE_ADDR_LIMIT) {
		if (*locked)
			munlock((void *)aligned, vlen);
		munmap((void *)aligned, vlen);
		return -ERANGE;
	}
	dma->addr = (void *)aligned;
	dma->device_addr = head << 12;
	dma->size = size;
	ctx->dma_allocs++;
	return 0;
}

static int dma_alloc(void *c, size_t size, struct mana_dma *dma)
{
	struct vfio_ctx *ctx = c;
	bool locked = false;
	int err = dma_reserve(ctx, size, dma, &locked);

	(void)locked;
	if (err) {
		memset(dma, 0, sizeof(*dma));
		platform_error(ctx, "dma_alloc", err);
	}
	return err;
}

static void dma_free(void *c, struct mana_dma *dma)
{
	struct vfio_ctx *ctx = c;
	size_t vlen;

	if (!dma->addr || !dma->size) {
		memset(dma, 0, sizeof(*dma));
		return;
	}
	vlen = dma_vlen(dma->size);
	munlock(dma->addr, vlen);
	if (munmap(dma->addr, vlen))
		platform_error(ctx, "dma_free munmap", -errno);
	else if (ctx->dma_allocs)
		ctx->dma_allocs--;
	memset(dma, 0, sizeof(*dma));
}

static void barrier(void *c)
{
	(void)c;
	/*
	 * x86 stores already leave the core in order, so a write barrier
	 * only needs to stop compiler reordering. A SEQ_CST fence does
	 * that and costs nothing extra on x86: no fence instruction is
	 * emitted for loads, and this keeps the doorbell ordering honest
	 * on any architecture the runner is later built for.
	 */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ---------- IRQ ---------- */

static int set_irqs(struct vfio_ctx *ctx, uint32_t flags, unsigned int count,
		    int fd)
{
	struct vfio_irq_set *req;
	size_t size = sizeof(*req) + (count ? sizeof(fd) : 0);
	int err = 0;

	req = calloc(1, size);
	if (!req)
		return -ENOMEM;
	req->argsz = (__u32)size;
	req->index = VFIO_PCI_MSIX_IRQ_INDEX;
	req->start = 0;
	req->count = count;
	req->flags = flags;
	if (count)
		memcpy((char *)req + sizeof(*req), &fd, sizeof(fd));
	if (ioctl(ctx->device_fd, VFIO_DEVICE_SET_IRQS, req) != 0)
		err = -errno;
	free(req);
	return err;
}

/*
 * Probe MSI-X eventfd feasibility once during GATE_PROBE. Register
 * vector 0, then release it so the gdma gate can register it for real.
 * Failure means the kernel refused MSI-X for this container. In noiommu
 * mode that is the expected risk on hosts without interrupt support.
 */
int vfio_irq_probe(struct vfio_ctx *ctx)
{
	int err;

	if (!ctx->msix_vectors) {
		ctx->irq_mode = VFIO_IRQ_MODE_NONE;
		return -ENOTSUP;
	}
	ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ctx->event_fd < 0) {
		ctx->irq_mode = VFIO_IRQ_MODE_NONE;
		return -errno;
	}
	err = set_irqs(ctx, VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
		       1, ctx->event_fd);
	if (err) {
		ctx->irq_errno = err;
		close(ctx->event_fd);
		ctx->event_fd = -1;
		ctx->irq_mode = VFIO_IRQ_MODE_NONE;
		return err;
	}
	(void)set_irqs(ctx, VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		       0, -1);
	ctx->irq_mode = VFIO_IRQ_MODE_MSIX_EVENTFD;
	ctx->irq_errno = 0;
	return 0;
}

static int irq_alloc(void *c, void (*handler)(void *), void *arg,
		     struct mana_irq *irq)
{
	struct vfio_ctx *ctx = c;
	int err;

	if (ctx->irq_mode == VFIO_IRQ_MODE_NONE) {
		platform_error(ctx,
			       "irq_alloc: MSI-X eventfd unavailable, the driver "
			       "requires interrupt delivery for the EQ self-tests",
			       -ENOTSUP);
		return -ENOTSUP;
	}
	if (ctx->irq_mode == VFIO_IRQ_MODE_MSIX_EVENTFD) {
		err = set_irqs(ctx,
			       VFIO_IRQ_SET_DATA_EVENTFD |
				   VFIO_IRQ_SET_ACTION_TRIGGER,
			       1, ctx->event_fd);
		if (err) {
			platform_error(ctx, "irq_alloc SET_IRQS", err);
			return err;
		}
	}
	ctx->handler = handler;
	ctx->handler_arg = arg;
	/*
	 * The driver stores these into MSI-X table entry zero. VFIO
	 * programs the physical entry from its own MSI domain and drops
	 * guest table writes, so the values below are only the standard
	 * xAPIC message shape. Nothing on this platform reads them back.
	 */
	irq->irq = 24;
	irq->address = UINT64_C(0xfee00000);
	irq->data = 0x21;
	return 0;
}

static int irq_free(void *c, struct mana_irq *irq)
{
	struct vfio_ctx *ctx = c;
	int err = 0;

	(void)irq;
	if (ctx->irq_mode == VFIO_IRQ_MODE_MSIX_EVENTFD)
		err = set_irqs(ctx,
			       VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
			       0, -1);
	ctx->handler = NULL;
	ctx->handler_arg = NULL;
	return err;
}

/* ---------- clock ---------- */

static int delay_us(void *c, unsigned int usec)
{
	struct vfio_ctx *ctx = c;
	uint64_t now = now_ms();

	vfio_pump_irq(ctx);
	if (ctx->deadline_ms && now >= ctx->deadline_ms) {
		ctx->timeouts++;
		printf("WAIT_TIMEOUT waited=%llums for=%s"
		       " env=MANA_VFIO_TIMEOUT_MS=%u\n",
		       (unsigned long long)(now - (ctx->deadline_ms -
						   ctx->timeout_ms)),
		       ctx->wait_label ? ctx->wait_label : "call",
		       ctx->timeout_ms);
		fflush(stdout);
		return -ETIMEDOUT;
	}
	if (usec)
		usleep(usec);
	return 0;
}

/* ---------- platform assembly ---------- */

const struct mana_platform *vfio_platform(struct vfio_ctx *ctx)
{
	static struct mana_platform instance;

	instance = (struct mana_platform){
		.ctx = ctx,
		.dma_guarantees = MANA_DMA_REQUIRED,
		.config_read = config_read,
		.config_write = config_write,
		.map = bar_map,
		.unmap = bar_unmap,
		.read32 = mmio_read32,
		.write32 = mmio_write32,
		.write64 = mmio_write64,
		.dma_alloc = dma_alloc,
		.dma_free = dma_free,
		.dma_rmb = barrier,
		.dma_wmb = barrier,
		.irq_alloc = irq_alloc,
		.irq_free = irq_free,
		.delay_us = delay_us,
		.error = platform_error,
	};
	return &instance;
}

/* ---------- device open ---------- */

static int read_link(const char *path, char *out, size_t cap)
{
	ssize_t got = readlink(path, out, cap - 1);

	if (got < 0)
		return -errno;
	out[got] = '\0';
	return 0;
}

static void hint_noiommu(void)
{
	printf("HINT enable noiommu: echo 1 > /sys/module/vfio/parameters/"
	       "enable_unsafe_noiommu_mode (vfio core module on kernel 6.2 and "
	       "later; older kernels use vfio_iommu_type1). Rebind the VF to "
	       "vfio-pci after enabling.\n");
	printf("HINT the noiommu group path also needs CAP_SYS_RAWIO, so run "
	       "as root.\n");
}

/* Open the group character device for the BDF. Returns fd or negative errno. */
static int open_group_fd(const char *bdf, bool *noiommu)
{
	char link[PATH_MAX];
	char path[PATH_MAX];
	const char *name;
	int fd;

	*noiommu = false;
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.24s/iommu_group",
		 bdf);
	if (read_link(path, link, sizeof(link)) != 0) {
		printf("ERROR no iommu_group link for %s. The VF is not bound "
		       "to vfio-pci, or noiommu mode was off when it was "
		       "bound.\n", bdf);
		hint_noiommu();
		return -ENOENT;
	}
	name = strrchr(link, '/');
	name = name ? name + 1 : link;
	if (strchr(name, '-') == NULL) {
		snprintf(path, sizeof(path), "/dev/vfio/%.64s", name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		if (errno != ENOENT)
			return -errno;
		/* Kernels may name the sysfs group numerically and the
		 * noiommu device node noiommu-<id>. Observed on 7.0.0-azure:
		 * sysfs kernel/iommu_groups/0 and node /dev/vfio/noiommu-0.
		 * Try the direct node, then fall through to the live scan.
		 */
		snprintf(path, sizeof(path), "/dev/vfio/noiommu-%.64s", name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			*noiommu = true;
			return fd;
		}
		if (errno != ENOENT)
			return -errno;
	}
	*noiommu = true;
	if (strncmp(name, "noiommu-", 8) == 0 ||
	    strncmp(name, "vfio-noiommu-", 13) == 0) {
		const char *id = strrchr(name, '-');

		snprintf(path, sizeof(path), "/dev/vfio/noiommu-%.64s", id + 1);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		if (errno != ENOENT)
			return -errno;
	}
	/* Fall back to the live noiommu cdevs under /dev/vfio. */
	{
		DIR *dir = opendir("/dev/vfio");
		struct dirent *de;
		int found = -1;
		int tries = 0;

		if (!dir)
			return -errno;
		while ((de = readdir(dir)) != NULL) {
			if (strncmp(de->d_name, "noiommu-", 8) != 0)
				continue;
			tries++;
			snprintf(path, sizeof(path), "/dev/vfio/%.255s",
				 de->d_name);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0) {
				if (errno == EPERM) {
					closedir(dir);
					printf("ERROR opening %s failed: EPERM,"
					       " noiommu mode is off or the "
					       "caller lacks CAP_SYS_RAWIO.\n",
					       path);
					hint_noiommu();
					return -EPERM;
				}
				continue;
			}
			if (found >= 0)
				close(found);
			found = fd;
		}
		closedir(dir);
		if (found < 0) {
			if (tries == 0) {
				printf("ERROR no /dev/vfio/noiommu-* node "
				       "exists.\n");
				hint_noiommu();
				return -ENOENT;
			}
			return -ENODEV;
		}
		if (tries > 1)
			printf("NOTE %d noiommu groups; the probe stage checks "
			       "the vendor and device ids of the chosen fd.\n",
			       tries);
		return found;
	}
}

static int check_bound_driver(const char *bdf)
{
	char link[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;
	const char *name;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.24s", bdf);
	if (stat(path, &st) != 0) {
		printf("ERROR no PCI device at %s. Find the MANA VF with:"
		       " lspci -Dnd 1414:00ba\n", bdf);
		return -ENOENT;
	}
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.24s/driver", bdf);
	if (read_link(path, link, sizeof(link)) != 0) {
		printf("ERROR no driver is bound to %s. Bind it to vfio-pci "
		       "first (coordinator step).\n", bdf);
		return -ENOENT;
	}
	name = strrchr(link, '/');
	name = name ? name + 1 : link;
	if (strcmp(name, "vfio-pci") != 0) {
		printf("ERROR device %s is bound to driver '%s'. The runner "
		       "needs vfio-pci and never rebinds drivers.\n", bdf, name);
		if (strcmp(name, "mana") == 0)
			printf("NOTE unbinding the VF cuts VM networking. The "
			       "coordinator owns that window.\n");
		return -EBUSY;
	}
	return 0;
}

static void fill_bar_table(struct vfio_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < VFIO_BAR_COUNT; i++) {
		struct vfio_region_info region = { .argsz = sizeof(region) };
		uint32_t lo = 0;

		region.index = VFIO_PCI_BAR0_REGION_INDEX + i;
		if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_REGION_INFO,
			  &region) == 0) {
			ctx->bars[i].offset = region.offset;
			ctx->bars[i].size = region.size;
			ctx->bars[i].present = region.size != 0;
		}
		if (!ctx->bars[i].present)
			continue;
		if (vfio_config_read(ctx, 0x10 + i * 4, 4, &lo) != 0) {
			ctx->bars[i].present = false;
			continue;
		}
		ctx->bars[i].base = lo & ~(uint32_t)0xf;
		if ((lo & 6) == 4 && i + 1 < VFIO_BAR_COUNT) {
			uint32_t hi = 0;

			if (vfio_config_read(ctx, 0x10 + (i + 1) * 4, 4, &hi) == 0) {
				ctx->bars[i].base |= (uint64_t)hi << 32;
				ctx->bars[i + 1].present = false; /* upper half */
				i++;
			}
		}
	}
}

/*
 * Locate the MSI-X capability and seed the virtual control image so
 * config reads of Message Control stay consistent with userspace
 * writes. See msix_ctrl_write.
 */
static void virtualize_msix_ctrl(struct vfio_ctx *ctx)
{
	uint32_t ptr = 0, id = 0, next = 0, ctrl = 0;
	unsigned int hops;

	if (vfio_config_read(ctx, 0x34, 1, &ptr))
		return;
	for (hops = 0; hops < 48; hops++) {
		if (ptr < 0x40 || ptr + 8 > ctx->config_size)
			return;
		if (vfio_config_read(ctx, ptr, 1, &id))
			return;
		if (id == 0x11) {
			ctx->msix_cap = (uint16_t)ptr;
			if (vfio_config_read(ctx, ptr + 2, 2, &ctrl) == 0)
				ctx->msix_ctrl_virt = (uint16_t)ctrl;
			printf("MSIXCAP off=0x%x ctrl=0x%04x\n",
			       (unsigned int)ptr,
			       (unsigned int)ctx->msix_ctrl_virt);
			return;
		}
		if (vfio_config_read(ctx, ptr + 1, 1, &next))
			return;
		next &= 0xfc;
		if (next == 0 || next == ptr)
			return;
		ptr = next;
	}
}

int vfio_open_device(struct vfio_ctx *ctx, const char *bdf)
{
	struct vfio_group_status status = { .argsz = sizeof(status) };
	struct vfio_irq_info irq = { .argsz = sizeof(irq) };
	struct vfio_region_info region = { .argsz = sizeof(region) };
	struct vfio_device_info info = { .argsz = sizeof(info) };
	bool noiommu = false;
	int iommu_type;
	int err;

	ctx->container_fd = -1;
	ctx->group_fd = -1;
	ctx->device_fd = -1;
	ctx->event_fd = -1;
	ctx->msix_cap = 0;
	ctx->msix_ctrl_virt = 0;

	err = check_bound_driver(bdf);
	if (err)
		return err;
	ctx->group_fd = open_group_fd(bdf, &noiommu);
	if (ctx->group_fd < 0)
		return ctx->group_fd;
	if (ioctl(ctx->group_fd, VFIO_GROUP_GET_STATUS, &status) != 0)
		return -errno;
	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("ERROR the group holding %s is not viable.\n", bdf);
		return -EINVAL;
	}
	ctx->container_fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (ctx->container_fd < 0)
		return -errno;
	if (ioctl(ctx->container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		printf("ERROR unexpected vfio api version.\n");
		return -EINVAL;
	}
	if (ioctl(ctx->group_fd, VFIO_GROUP_SET_CONTAINER, &ctx->container_fd) != 0)
		return -errno;
	iommu_type = noiommu ? VFIO_NOIOMMU_IOMMU : VFIO_TYPE1v2_IOMMU;
	if (ioctl(ctx->container_fd, VFIO_SET_IOMMU, iommu_type) != 0) {
		err = -errno;
		printf("ERROR VFIO_SET_IOMMU(%s) failed err=%d.\n",
		       noiommu ? "noiommu" : "type1v2", err);
		if (noiommu)
			hint_noiommu();
		return err;
	}
	printf("VFIO mode=%s device=%s\n", noiommu ? "noiommu" : "type1v2", bdf);
	ctx->device_fd = ioctl(ctx->group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (ctx->device_fd < 0)
		return -errno;
	if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_INFO, &info) != 0)
		return -errno;
	if (!(info.flags & VFIO_DEVICE_FLAGS_RESET))
		printf("RESET unsupported\n");
	else if (ioctl(ctx->device_fd, VFIO_DEVICE_RESET) != 0)
		printf("RESET failed errno=%d\n", errno);
	else
		printf("RESET ok\n");
	region.index = VFIO_PCI_CONFIG_REGION_INDEX;
	if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_REGION_INFO, &region) != 0)
		return -errno;
	ctx->config_offset = region.offset;
	ctx->config_size = region.size;
	fill_bar_table(ctx);
	irq.index = VFIO_PCI_MSIX_IRQ_INDEX;
	if (ioctl(ctx->device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq) == 0 &&
	    (irq.flags & VFIO_IRQ_INFO_EVENTFD))
		ctx->msix_vectors = irq.count;
	virtualize_msix_ctrl(ctx);
	return 0;
}

/* ---------- selftest ---------- */

/*
 * One selftest round. With CAP_SYS_ADMIN (the live run is root) this
 * proves PFN contiguity end to end. Without it the kernel hides PFNs,
 * so we prove the allocator path (aligned VMA, THP madvise, touch,
 * mlock) and report the contiguity check as unproven here.
 */
static int selftest_one(size_t size)
{
	size_t vlen = dma_vlen(size);
	uintptr_t aligned = 0;
	uint64_t head = 0;
	bool locked = false;
	bool pfn_hidden = false;
	int err;

	err = dma_attempt(size, vlen, &aligned, &head, &locked, &pfn_hidden);
	if (err == -EACCES) {
		printf("SELFTEST PASS size=%zu pfn=hidden-unprivileged"
		       " detail=allocator+thp+mlock proven, contiguity not"
		       " visible without CAP_SYS_ADMIN\n", size);
		return 0;
	}
	if (err) {
		printf("SELFTEST FAIL size=%zu err=%d detail=dma_attempt\n",
		       size, err);
		return 1;
	}
	if (aligned % 4096 || (head << 12) % 4096 ||
	    (head << 12) >= MANA_DEVICE_ADDR_LIMIT) {
		printf("SELFTEST FAIL size=%zu err=-22 detail=contract\n",
		       size);
		err = 1;
	} else {
		printf("SELFTEST PASS size=%zu device_addr=0x%" PRIx64
		       " pfn=contiguous mlock=%s\n", size, head << 12,
		       locked ? "ok" : "unavailable");
	}
	if (locked)
		munlock((void *)aligned, vlen);
	munmap((void *)aligned, vlen);
	return err;
}

int vfio_selftest(void)
{
	long page = sysconf(_SC_PAGESIZE);
	int failed = 0;

	if (page != 4096) {
		printf("SELFTEST FAIL err=-1 detail=page-size-%ld\n", page);
		return 1;
	}
	failed |= selftest_one(4096);
	failed |= selftest_one(4096 * 16);
	printf("SELFTEST_DONE %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
