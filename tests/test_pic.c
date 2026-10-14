/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <uk/arch/util.h>
#include <uk/asm/pic.h>
#include <uk/intctlr.h>
#include <pic.h>

static unsigned char ports[65536];
static unsigned int reads, writes;

__u8 uk_arch_x86_64_inb(__u16 port)
{
	reads++;
	return ports[port];
}

void uk_arch_x86_64_outb(__u16 port, __u8 value)
{
	writes++;
	ports[port] = value;
}

int main(void)
{
	static const unsigned int irqs[] = {0, 7, 8, 15, 16, 20, 40, 207, UINT_MAX};
	struct uk_intctlr_driver_ops *ops;
	size_t i;
	for (i = 0; i < sizeof(irqs) / sizeof(irqs[0]); i++) {
		unsigned int irq = irqs[i];
		memset(ports, 0, sizeof(ports));
		assert(pic_init(&ops) == 0);
		reads = writes = 0;
		ops->mask_irq(irq);
		if (irq < 16) {
			assert(reads == 1 && writes == 1);
			assert(ports[IRQ_PORT(irq)] == (1U << IRQ_OFFSET(irq)));
		}
		ops->unmask_irq(irq);
		pic_ack_irq(irq);
		if (irq < 16) {
			assert(reads == 2 && writes == (irq < 8 ? 3U : 4U));
			assert(ports[IRQ_PORT(irq)] == 0);
			assert(ports[PIC1_COMMAND] == PIC_EOI);
			if (irq >= 8)
				assert(ports[PIC2_COMMAND] == PIC_EOI);
		} else {
			assert(reads == 0 && writes == 0);
		}
	}
	puts("MANA PIC guard: PASS (actual patched source, IRQs 15/16/20/207, UBSan)");
	return 0;
}
