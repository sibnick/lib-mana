/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_INTERNAL_H
#define MANA_INTERNAL_H

#include <errno.h>
#include <string.h>
#include <uk/mana.h>
#include <uk/mana/gdma.h>
#include <uk/mana/bootstrap.h>

static inline uint16_t mana_get16(const void *v)
{
	const unsigned char *p = v;
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline void mana_put16(void *v, uint16_t n)
{
	unsigned char *p = v;
	p[0] = (unsigned char)n;
	p[1] = (unsigned char)(n >> 8);
}

static inline uint32_t mana_get32(const void *v)
{
	const unsigned char *p = v;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t mana_get64(const void *v)
{
	const unsigned char *p = v;
	return mana_get32(p) | (uint64_t)mana_get32(p + 4) << 32;
}

static inline void mana_put32(void *v, uint32_t n)
{
	unsigned char *p = v;
	unsigned int i;
	for (i = 0; i < 4; i++)
		p[i] = (unsigned char)(n >> (8 * i));
}

static inline void mana_put64(void *v, uint64_t n)
{
	mana_put32(v, (uint32_t)n);
	mana_put32((unsigned char *)v + 4, (uint32_t)(n >> 32));
}

static inline int mana_range(uint64_t size, uint64_t off, uint64_t length)
{
	return off <= size && length <= size - off;
}

static inline int mana_overlap(uint64_t a, uint64_t a_size,
			       uint64_t c, uint64_t c_size)
{
	return a_size && c_size &&
	       (a <= c ? c - a < a_size : a - c < c_size);
}

static inline uint32_t mana_r32(struct mana_bootstrap *b, uint64_t off)
{
	return b->p->read32(b->p->ctx, &b->bar0, off);
}

static inline uint64_t mana_r64(struct mana_bootstrap *b, uint64_t off)
{
	uint32_t lo = mana_r32(b, off);
	return lo | (uint64_t)mana_r32(b, off + 4) << 32;
}

int mana_pci_start(struct mana_bootstrap *b);
int mana_pci_stop(struct mana_bootstrap *b);
int mana_pci_command(struct mana_bootstrap *b, uint16_t command);
int mana_gdma_start(struct mana_bootstrap *b);
int mana_gdma_destroy(struct mana_bootstrap *b);

#endif
