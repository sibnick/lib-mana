/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static int cfg_read(struct mana_bootstrap *b, uint16_t off, unsigned int width,
		    uint32_t *v)
{
	return b->p->config_read(b->p->ctx, off, width, v);
}

static int cfg_write(struct mana_bootstrap *b, uint16_t off, unsigned int width,
		     uint32_t v)
{
	return b->p->config_write(b->p->ctx, off, width, v);
}

static void irq_event(void *arg)
{
	struct mana_bootstrap *b = arg;
	b->interrupts++;
}

int mana_pci_command(struct mana_bootstrap *b, uint16_t command)
{
	uint32_t v;
	int err = cfg_write(b, 4, 2, command);
	if (!err)
		err = cfg_read(b, 4, 2, &v);
	if (err)
		return err;
	return ((v ^ command) & 0x407) ? -EIO : 0;
}

static int read_bars(struct mana_bootstrap *b)
{
	unsigned int i, j;
	int err, restore;
	uint32_t lo, hi, masklo, maskhi;
	uint64_t base, mask, size;

	for (i = 0; i < 6; i++) {
		uint16_t off = (uint16_t)(0x10 + i * 4);
		int wide;
		err = cfg_read(b, off, 4, &lo);
		if (err)
			return err;
		wide = !(lo & 1) && (lo & 6) == 4;
		if (wide && i == 5)
			return -EPROTO;
		hi = 0;
		if (wide && (err = cfg_read(b, off + 4, 4, &hi)))
			return err;
		b->bar_saved[i] = lo;
		b->bar_dirty |= 1U << i;
		if (wide) {
			b->bar_saved[i + 1] = hi;
			b->bar_dirty |= 1U << (i + 1);
		}
		err = cfg_write(b, off, 4, UINT32_MAX);
		if (!err && wide)
			err = cfg_write(b, off + 4, 4, UINT32_MAX);
		if (!err)
			err = cfg_read(b, off, 4, &masklo);
		maskhi = 0;
		if (!err && wide)
			err = cfg_read(b, off + 4, 4, &maskhi);
		restore = cfg_write(b, off, 4, lo);
		if (!restore)
			b->bar_dirty &= ~(1U << i);
		if (wide) {
			int r = cfg_write(b, off + 4, 4, hi);
			if (!r)
				b->bar_dirty &= ~(1U << (i + 1));
			if (!restore)
				restore = r;
		}
		if (restore || err)
			return restore ? restore : err;
		if (masklo == 0) {
			if (wide)
				i++;
			continue;
		}
		/* Prefetchable BARs still receive uncached software mappings. */
		if ((lo & 1) || ((lo & 6) != 0 && !wide))
			return -EPROTO;
		base = (lo & ~UINT64_C(15)) | (uint64_t)hi << 32;
		mask = (masklo & ~UINT64_C(15)) | (uint64_t)maskhi << 32;
		if (!wide)
			mask |= UINT64_C(0xffffffff00000000);
		size = ~mask + 1;
		if (!base || !size || (size & (size - 1)) ||
		    (base & (size - 1)) || size < 4096 ||
		    base > UINT64_MAX - (size - 1))
			return -EPROTO;
		for (j = 0; j < i; j++) {
			if (mana_overlap(base, size, b->bars[j].physical, b->bars[j].size))
				return -EPROTO;
		}
		b->bars[i].physical = base;
		b->bars[i].size = size;
		if (wide)
			i++;
	}
	return b->bars[0].size ? 0 : -ENODEV;
}

static int find_msix(struct mana_bootstrap *b)
{
	uint32_t v, ptr;
	uint64_t seen = 0;
	int err;
	err = cfg_read(b, 6, 2, &v);
	if (err)
		return err;
	if (!(v & 0x10))
		return -ENOTSUP;
	err = cfg_read(b, 0x34, 1, &ptr);
	if (err)
		return err;
	while (ptr) {
		unsigned int bit;
		if (ptr < 0x40 || ptr > 0xfc || (ptr & 3))
			return -EPROTO;
		bit = ptr / 4;
		if (seen & (UINT64_C(1) << bit))
			return -EPROTO;
		seen |= UINT64_C(1) << bit;
		err = cfg_read(b, (uint16_t)ptr, 4, &v);
		if (err)
			return err;
		if ((v & 0xff) == 0x11) {
			if (ptr > 0xf4)
				return -EPROTO;
			b->msix_cap = (uint16_t)ptr;
			b->msix_saved_control = (uint16_t)(v >> 16);
			return (v & UINT32_C(0x80000000)) ? -EBUSY : 0;
		}
		ptr = (v >> 8) & 0xff;
	}
	return -ENOTSUP;
}

static int setup_msix(struct mana_bootstrap *b)
{
	uint32_t table, pba;
	uint32_t n = (b->msix_saved_control & 0x7ff) + 1;
	uint32_t bir;
	uint64_t off;
	unsigned int i;
	int err;

	err = cfg_read(b, b->msix_cap + 4, 4, &table);
	if (!err)
		err = cfg_read(b, b->msix_cap + 8, 4, &pba);
	if (err)
		return err;
	bir = pba & 7;
	off = pba & ~UINT64_C(7);
	if (bir >= 6 || !mana_range(b->bars[bir].size, off,
				    ((n + 63) / 64) * 8))
		return -EPROTO;
	b->pba_bir = bir;
	b->pba_offset = off;
	b->pba_length = ((n + 63) / 64) * 8;
	bir = table & 7;
	off = table & ~UINT64_C(7);
	if (bir >= 6 || !mana_range(b->bars[bir].size, off, (uint64_t)n * 16))
		return -EPROTO;
	b->table_bir = bir;
	b->table_length = (uint64_t)n * 16;
	if ((pba & 7) == bir) {
		uint64_t pba_off = pba & ~UINT64_C(7);
		if (off < pba_off + ((n + 63) / 64) * 8 &&
		    pba_off < off + (uint64_t)n * 16)
			return -EPROTO;
	}
	if ((!bir &&
	     (mana_overlap(off, b->table_length, 0, 32) ||
	      mana_overlap(off, b->table_length, b->shm, 32))) ||
	    (!b->pba_bir &&
	     (mana_overlap(b->pba_offset, b->pba_length, 0, 32) ||
	      mana_overlap(b->pba_offset, b->pba_length, b->shm, 32))))
		return -EPROTO;
	b->table_offset = off;
	if (bir == 0) {
		b->table = b->bar0;
	} else {
		err = b->p->map(b->p->ctx, b->bars[bir].physical,
			       b->bars[bir].size, &b->table);
		if (err)
			return err;
		b->table_owned = 1;
	}
	if (!b->table.addr || b->table.size != b->bars[bir].size)
		return -EPROTO;
	for (i = 0; i < 4; i++)
		b->msix_saved[i] = b->p->read32(b->p->ctx, &b->table, off + i * 4);
	for (i = 1; i < n; i++) {
		if (!(b->p->read32(b->p->ctx, &b->table, off + i * 16 + 12) & 1))
			return -EBUSY;
	}
	err = cfg_write(b, b->msix_cap + 2, 2,
			b->msix_saved_control | 0x4000);
	if (err)
		return err;
	err = b->p->irq_alloc(b->p->ctx, irq_event, b, &b->irq);
	if (err)
		return err;
	b->irq_owned = 1;
	b->p->write32(b->p->ctx, &b->table, off + 12, 1);
	b->p->write32(b->p->ctx, &b->table, off, (uint32_t)b->irq.address);
	b->p->write32(b->p->ctx, &b->table, off + 4,
		      (uint32_t)(b->irq.address >> 32));
	b->p->write32(b->p->ctx, &b->table, off + 8, b->irq.data);
	b->p->dma_wmb(b->p->ctx);
	(void)b->p->read32(b->p->ctx, &b->table, off + 8);
	b->p->write32(b->p->ctx, &b->table, off + 12, 0);
	return cfg_write(b, b->msix_cap + 2, 2,
			 (b->msix_saved_control | 0x8000) & ~0x4000U);
}

int mana_pci_start(struct mana_bootstrap *b)
{
	uint32_t v;
	int err;
	err = cfg_read(b, 0, 4, &v);
	if (err)
		return err;
	if (v != (MANA_PCI_VENDOR_ID | MANA_PCI_VF_DEVICE_ID << 16))
		return -ENODEV;
	err = cfg_read(b, 0x0e, 1, &v);
	if (err)
		return err;
	if ((v & 0x7f) != 0)
		return -EPROTO;
	err = cfg_read(b, 4, 2, &v);
	if (err)
		return err;
	/* Do not take over a device that can already issue DMA. */
	if (v & 4)
		return -EBUSY;
	b->command_saved = (uint16_t)v;
	b->config_owned = 1;
	err = mana_pci_command(b, (uint16_t)(v & ~3U));
	if (!err)
		err = read_bars(b);
	if (!err)
		err = find_msix(b);
	if (!err)
		err = mana_pci_command(b,
			(uint16_t)((b->command_saved | 2 | 0x400) & ~4U));
	if (!err)
		err = b->p->map(b->p->ctx, b->bars[0].physical,
			       b->bars[0].size, &b->bar0);
	if (err)
		return err;
	if (!b->bar0.addr || b->bar0.size != b->bars[0].size ||
	    b->bar0.size < 0x20)
		return -EPROTO;
	b->db_size = mana_r32(b, 0x10) & 0xffff;
	b->db = mana_r64(b, 8);
	b->shm = mana_r64(b, 0x18);
	if (b->db_size < 4096 || (b->db_size & 4095) || (b->db & 4095) ||
	    !mana_range(b->bar0.size, b->db, b->db_size) ||
	    (b->shm & 3) || !mana_range(b->bar0.size, b->shm, 32) ||
	    b->shm < 0x20 || b->db < 0x20 ||
	    (b->shm < b->db + b->db_size && b->db < b->shm + 32))
		return -EPROTO;
	return setup_msix(b);
}

int mana_pci_stop(struct mana_bootstrap *b)
{
	unsigned int i;
	int err;
	for (i = 0; i < 6; i++) {
		if (!(b->bar_dirty & (1U << i)))
			continue;
		err = cfg_write(b, (uint16_t)(0x10 + i * 4), 4, b->bar_saved[i]);
		if (err)
			return err;
		b->bar_dirty &= ~(1U << i);
	}
	if (b->irq_owned) {
		err = cfg_write(b, b->msix_cap + 2, 2,
				b->msix_saved_control | 0x4000);
		if (err)
			return err;
		b->p->write32(b->p->ctx, &b->table, b->table_offset + 12, 1);
		(void)b->p->read32(b->p->ctx, &b->table, b->table_offset + 12);
		err = b->p->irq_free(b->p->ctx, &b->irq);
		if (err)
			return err;
		b->irq_owned = 0;
		for (i = 0; i < 4; i++)
			b->p->write32(b->p->ctx, &b->table,
				      b->table_offset + i * 4, b->msix_saved[i]);
	}
	if (b->msix_cap && !(b->msix_saved_control & 0x8000)) {
		err = cfg_write(b, b->msix_cap + 2, 2, b->msix_saved_control);
		if (err)
			return err;
		b->msix_cap = 0;
	}
	if (b->table_owned) {
		err = b->p->unmap(b->p->ctx, &b->table);
		if (err)
			return err;
		b->table_owned = 0;
	}
	if (b->bar0.addr) {
		err = b->p->unmap(b->p->ctx, &b->bar0);
		if (err)
			return err;
	}
	if (b->config_owned) {
		err = mana_pci_command(b, b->command_saved);
		if (err)
			return err;
		b->config_owned = 0;
	}
	return 0;
}
