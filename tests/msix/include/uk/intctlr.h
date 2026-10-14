/* SPDX-License-Identifier: BSD-3-Clause */
#include <stddef.h>
struct uk_intctlr_driver_ops {
	void *fdt_xlat;
	void (*mask_irq)(unsigned int);
	void (*unmask_irq)(unsigned int);
};
int uk_intctlr_irq_alloc(unsigned int *irq, size_t count);
int uk_intctlr_irq_free(unsigned int *irq, size_t count);
