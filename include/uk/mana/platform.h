/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_PLATFORM_H
#define UK_MANA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define MANA_DMA_COHERENT 1U
#define MANA_DMA_DEVICE_ADDRESS 2U
#define MANA_DMA_UNENCRYPTED 4U
#define MANA_DMA_REQUIRED 7U

struct mana_mapping {
	void *addr;
	uint64_t size;
};

struct mana_dma {
	void *addr;
	uint64_t device_addr;
	size_t size;
};

struct mana_irq {
	unsigned int irq;
	uint64_t address;
	uint32_t data;
};

/* Operations return zero or negative errno. Failed allocations own nothing.
 * DMA allocations return zeroed, contiguous, 4 KiB aligned coherent memory.
 * MMIO mappings use uncached device attributes, not ordinary RAM mappings.
 * The IRQ callback only records delivery. Protocol work runs outside IRQs.
 */
struct mana_platform {
	void *ctx;
	unsigned int dma_guarantees;
	int (*config_read)(void *ctx, uint16_t off, unsigned int width,
			   uint32_t *value);
	int (*config_write)(void *ctx, uint16_t off, unsigned int width,
			    uint32_t value);
	int (*map)(void *ctx, uint64_t physical, uint64_t size,
		   struct mana_mapping *mapping);
	int (*unmap)(void *ctx, struct mana_mapping *mapping);
	uint32_t (*read32)(void *ctx, const struct mana_mapping *m, uint64_t off);
	void (*write32)(void *ctx, const struct mana_mapping *m, uint64_t off,
			uint32_t value);
	void (*write64)(void *ctx, const struct mana_mapping *m, uint64_t off,
			uint64_t value);
	int (*dma_alloc)(void *ctx, size_t size, struct mana_dma *dma);
	void (*dma_free)(void *ctx, struct mana_dma *dma);
	void (*dma_rmb)(void *ctx);
	void (*dma_wmb)(void *ctx);
	int (*irq_alloc)(void *ctx, void (*handler)(void *), void *arg,
			 struct mana_irq *irq);
	int (*irq_free)(void *ctx, struct mana_irq *irq);
	int (*delay_us)(void *ctx, unsigned int usec);
	void (*error)(void *ctx, const char *stage, int err);
};

#endif
