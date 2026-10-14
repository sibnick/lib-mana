/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <uk/alloc.h>
#include <uk/arch/util.h>
#include <uk/bus/pci.h>
#include <uk/intctlr.h>
#include <uk/intctlr/msix.h>
#include <uk/init.h>
#include <uk/lcpu/except.h>
#include <uk/paging.h>
#include <uk/plat/time.h>
#include <uk/print.h>
#include <uk/vmem.h>
#include <uk/vmem/vma_types.h>

struct mana_uk_context {
	struct pci_device *device;
	struct uk_alloc *alloc;
	struct mana_platform platform;
	struct mana_bootstrap bootstrap;
	void (*handler)(void *);
	void *handler_arg;
	unsigned int irq_registered;
	unsigned int claimed;
};

/* The pinned PCI bus has no driver-data or remove callbacks. */
static struct mana_uk_context mana_uk;

static int config_access(struct mana_uk_context *c, uint16_t off,
			 unsigned int width, uint32_t *v, int write)
{
	uint32_t address;
	unsigned long flags;
	if (!c->device || c->device->addr.domain || off >= 256 ||
	    (width != 1 && width != 2 && width != 4) ||
	    (off & (width - 1)) || width > 256U - off)
		return -EINVAL;
	address = UINT32_C(0x80000000) |
		  (uint32_t)c->device->addr.bus << 16 |
		  (uint32_t)c->device->addr.devid << 11 |
		  (uint32_t)c->device->addr.function << 8 | (off & ~3U);
	flags = uk_lcpu_save_irqf();
	uk_arch_x86_64_outl(0xcf8, address);
	if (write) {
		if (width == 1)
			uk_arch_x86_64_outb((uint16_t)(0xcfc + (off & 3)), (__u8)*v);
		else if (width == 2)
			uk_arch_x86_64_outw((uint16_t)(0xcfc + (off & 3)), (__u16)*v);
		else
			uk_arch_x86_64_outl(0xcfc, *v);
	} else {
		if (width == 1)
			*v = uk_arch_x86_64_inb((uint16_t)(0xcfc + (off & 3)));
		else if (width == 2)
			*v = uk_arch_x86_64_inw((uint16_t)(0xcfc + (off & 3)));
		else
			*v = uk_arch_x86_64_inl(0xcfc);
	}
	uk_lcpu_restore_irqf(flags);
	return 0;
}

static int config_read(void *ctx, uint16_t off, unsigned int width, uint32_t *v)
{
	return config_access(ctx, off, width, v, 0);
}

static int config_write(void *ctx, uint16_t off, unsigned int width, uint32_t v)
{
	return config_access(ctx, off, width, &v, 1);
}

static int map(void *ctx, uint64_t physical, uint64_t size,
	       struct mana_mapping *m)
{
	__vaddr_t va = UK_PAGING_VADDR_ANY;
	__u32 a, b, c, d;
	unsigned int bits;
	int err;
	(void)ctx;
	if (!size || (size & 4095) || (physical & 4095) ||
	    physical > UINT64_MAX - size || size > SIZE_MAX ||
	    !uk_vas_get_active())
		return -EINVAL;
	uk_arch_x86_64_cpuid(0x80000000U, 0, &a, &b, &c, &d);
	if (a < 0x80000008U)
		return -ENOTSUP;
	uk_arch_x86_64_cpuid(0x80000008U, 0, &a, &b, &c, &d);
	bits = a & 0xff;
	if (bits < 32 || bits > 52 || ((physical + size - 1) >> bits))
		return -ERANGE;
	/* Despite its name, this pinned attribute selects PAT index 3.
	 * The pinned platform programs that index as UC. Check the live PAT.
	 */
	if (((uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_MSR_PAT) >> 24) & 0xff) !=
	    UK_ARCH_X86_64_PAT_UC)
		return -ENOTSUP;
	err = uk_vma_map_dma(uk_vas_get_active(), &va, (size_t)size,
			    UK_PAGING_PAGE_ATTR_PROT_RW |
			    UK_ARCH_X86_64_PAGE_ATTR_WRITECOMBINE,
			    UK_VMA_MAP_POPULATE | UK_VMA_MAP_SIZE_4KB,
			    "MANA MMIO", physical);
	if (err)
		return err;
	m->addr = (void *)va;
	m->size = size;
	return 0;
}

static int unmap(void *ctx, struct mana_mapping *m)
{
	int err;
	(void)ctx;
	err = uk_vma_unmap(uk_vas_get_active(), (__vaddr_t)m->addr, m->size, 0);
	if (!err)
		memset(m, 0, sizeof(*m));
	return err;
}

static uint32_t read32(void *ctx, const struct mana_mapping *m, uint64_t off)
{
	(void)ctx;
	return *(volatile uint32_t *)((unsigned char *)m->addr + off);
}

static void write32(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint32_t v)
{
	(void)ctx;
	*(volatile uint32_t *)((unsigned char *)m->addr + off) = v;
}

static void write64(void *ctx, const struct mana_mapping *m, uint64_t off,
		    uint64_t v)
{
	(void)ctx;
	*(volatile uint64_t *)((unsigned char *)m->addr + off) = v;
}

static int dma_alloc(void *ctx, size_t size, struct mana_dma *dma)
{
	struct mana_uk_context *c = ctx;
	void *va;
	__paddr_t pa;
	__pte_t pte;
	unsigned int level = UK_PAGING_PAGE_LEVEL;
	int err;
	if (size != 4096)
		return -EINVAL;
	va = uk_palloc(c->alloc, 1);
	if (!va)
		return -ENOMEM;
	memset(va, 0, size);
	err = uk_paging_pt_walk(uk_paging_pt_get_active(), (__vaddr_t)va,
				&level, NULL, &pte);
	if (err || !UK_PAGING_PT_Lx_PTE_PRESENT(pte, level) ||
	    !UK_PAGING_PAGE_Lx_IS(pte, level) ||
	    (pte & (UK_ARCH_X86_64_PTE_PCD | UK_ARCH_X86_64_PTE_PWT)) ||
	    (pte & (level == UK_PAGING_PAGE_LEVEL ? 0x80UL : 0x1000UL)) ||
	    (uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_MSR_PAT) & 0xff) !=
	    UK_ARCH_X86_64_PAT_WB) {
		uk_pfree(c->alloc, va, 1);
		return err ? err : -ENOTSUP;
	}
	pa = uk_paging_virt_to_phys((__vaddr_t)va);
	if (pa == UK_PAGING_PADDR_INV || (pa & 4095) ||
	    uk_paging_virt_to_phys((__vaddr_t)va + 4095) != pa + 4095) {
		uk_pfree(c->alloc, va, 1);
		return -EFAULT;
	}
	dma->addr = va;
	dma->device_addr = pa;
	dma->size = size;
	return 0;
}

static void dma_free(void *ctx, struct mana_dma *dma)
{
	struct mana_uk_context *c = ctx;
	uk_pfree(c->alloc, dma->addr, 1);
	memset(dma, 0, sizeof(*dma));
}

static void dma_rmb(void *ctx)
{
	(void)ctx;
	__asm__ __volatile__("lfence" ::: "memory");
}

static void dma_wmb(void *ctx)
{
	(void)ctx;
	__asm__ __volatile__("sfence" ::: "memory");
}

static int irq_handler(void *ctx)
{
	struct mana_uk_context *c = ctx;
	c->handler(c->handler_arg);
	return 1;
}

static int irq_alloc(void *ctx, void (*handler)(void *), void *arg,
		     struct mana_irq *irq)
{
	struct mana_uk_context *c = ctx;
	__u64 address;
	__u32 data;
	unsigned int vector;
	__u32 a, b, cpuc, d;
	uint64_t base;
	int err;
	uk_arch_x86_64_cpuid(1, 0, &a, &b, &cpuc, &d);
	if (!(cpuc & UK_ARCH_X86_64_CPUID1_ECX_X2APIC))
		return -ENOTSUP;
	base = uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_APIC_MSR_BASE);
	if ((base & (UK_ARCH_X86_64_APIC_BASE_EN | UK_ARCH_X86_64_APIC_BASE_EXTD)) !=
	    (UK_ARCH_X86_64_APIC_BASE_EN | UK_ARCH_X86_64_APIC_BASE_EXTD) ||
	    !(uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_APIC_MSR_SVR) &
	      UK_ARCH_X86_64_APIC_SVR_EN))
		return -ENOTSUP;
	if (uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_APIC_MSR_ID) != ((b >> 24) & 0xff))
		return -ENOTSUP;
	err = uk_intctlr_msix_alloc(0, &vector, &address, &data);
	if (err)
		return err;
	c->handler = handler;
	c->handler_arg = arg;
	err = uk_intctlr_irq_register(vector, irq_handler, c);
	if (err) {
		int free_err = uk_intctlr_msix_free(vector);
		if (free_err)
			uk_pr_err("MANA: IRQ allocation cleanup failed: %d\n", free_err);
		return err;
	}
	irq->irq = vector;
	irq->address = address;
	irq->data = data;
	c->irq_registered = 1;
	return 0;
}

static int irq_free(void *ctx, struct mana_irq *irq)
{
	struct mana_uk_context *c = ctx;
	int err;
	if (c->irq_registered) {
		err = uk_intctlr_irq_unregister(irq->irq, irq_handler);
		if (err)
			return err;
		c->irq_registered = 0;
	}
	return uk_intctlr_msix_free(irq->irq);
}

static int delay_us(void *ctx, unsigned int usec)
{
	uint64_t start = ukplat_monotonic_clock();
	uint64_t span = (uint64_t)usec * 1000;
	unsigned int i;
	(void)ctx;
	/* A stopped guest clock must not cause an unbounded wait. */
	for (i = 0; i < 1000000; i++) {
		if ((uint64_t)ukplat_monotonic_clock() - start >= span)
			return 0;
		__asm__ __volatile__("pause");
	}
	return -ETIMEDOUT;
}

static void error(void *ctx, const char *stage, int err)
{
	(void)ctx;
	uk_pr_err("MANA: %s: %d\n", stage, err);
}

static int mana_init(struct uk_alloc *alloc)
{
	mana_uk.alloc = alloc;
	return alloc ? 0 : -ENOMEM;
}

static int mana_add(struct pci_device *device)
{
	struct mana_uk_context *c = &mana_uk;
	int err;
	if (c->claimed)
		return -EBUSY;
	c->claimed = 1;
	c->device = device;
	c->platform = (struct mana_platform) {
		.ctx = c,
#if CONFIG_LIBMANA_DIRECT_DMA
		.dma_guarantees = MANA_DMA_REQUIRED,
#endif
		.config_read = config_read, .config_write = config_write,
		.map = map, .unmap = unmap,
		.read32 = read32, .write32 = write32, .write64 = write64,
		.dma_alloc = dma_alloc, .dma_free = dma_free,
		.dma_rmb = dma_rmb, .dma_wmb = dma_wmb,
		.irq_alloc = irq_alloc, .irq_free = irq_free,
		.delay_us = delay_us, .error = error,
	};
	if (uk_lcpu_irqs_disabled()) {
		uk_pr_err("MANA: HWC startup needs enabled interrupts\n");
		c->claimed = 0;
		c->device = NULL;
		return -ENOTSUP;
	}
	err = mana_bootstrap_start(&c->bootstrap, &c->platform);
	if (err) {
		/* PCI core frees its pci_device after add_dev returns an error.
		 * Keep its address in driver storage if DMA remains quarantined.
		 */
		if (c->bootstrap.quarantined) {
			static struct pci_device retained;
			retained = *device;
			c->device = &retained;
		} else {
			c->device = NULL;
			c->claimed = 0;
			memset(&c->bootstrap, 0, sizeof(c->bootstrap));
		}
		return err;
	}
	uk_pr_info("MANA: GDMA version 1 and HWC ready, MANA device 0x%x\n",
		   c->bootstrap.mana_device_id);
	device->state = PCI_DEVICE_STATE_RUNNING;
	return 0;
}

static const struct pci_device_id mana_ids[] = {
	{ PCI_DEVICE_ID(MANA_PCI_VENDOR_ID, MANA_PCI_VF_DEVICE_ID) },
	{ PCI_ANY_DEVICE_ID },
};

static struct pci_driver mana_driver = {
	.device_ids = mana_ids,
	.init = mana_init,
	.add_dev = mana_add,
};

PCI_REGISTER_DRIVER(&mana_driver);

struct mana_bootstrap *mana_unikraft_bootstrap(void)
{
	return mana_uk.claimed ? &mana_uk.bootstrap : NULL;
}

int mana_unikraft_stop(void)
{
	int err;
	if (!mana_uk.claimed)
		return 0;
	err = mana_bootstrap_stop(&mana_uk.bootstrap);
	if (!err) {
		if (mana_uk.device)
			mana_uk.device->state = PCI_DEVICE_STATE_RESET;
		mana_uk.claimed = 0;
		mana_uk.device = NULL;
		memset(&mana_uk.bootstrap, 0, sizeof(mana_uk.bootstrap));
	}
	return err;
}

static int shutdown_init(struct uk_init_ctx *ctx)
{
	(void)ctx;
	return 0;
}

static void shutdown_term(struct uk_term_ctx *ctx)
{
	(void)ctx;
	(void)mana_unikraft_stop();
}

uk_lib_initcall(shutdown_init, shutdown_term);
