/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <uk/arch/x86_64.h>
#include <uk/intctlr.h>
#include <uk/intctlr/limits.h>
#include <uk/intctlr/msix.h>

static unsigned int next_irq = UK_INTCTLR_FIRST_ALLOCABLE_IRQ;
static unsigned int alloc_calls;
static unsigned int free_calls;
static int alloc_error;

void uk_arch_x86_64_cpuid(__u32 leaf, __u32 subleaf, __u32 *a, __u32 *b,
			 __u32 *c, __u32 *d)
{
	assert(leaf == 1 && subleaf == 0);
	*a = *c = *d = 0;
	*b = 0x5a000000U;
}

int uk_intctlr_irq_alloc(unsigned int *irq, size_t count)
{
	assert(count == 1);
	alloc_calls++;
	if (alloc_error)
		return alloc_error;
	*irq = next_irq;
	return 0;
}

int uk_intctlr_irq_free(unsigned int *irq, size_t count)
{
	assert(count == 1);
	assert(*irq >= 20 && *irq <= 207);
	free_calls++;
	return 0;
}

int main(void)
{
	unsigned int irq = 999;
	__u64 addr = 0x1234;
	__u32 data = 0x5678;

	assert(UK_INTCTLR_FIRST_ALLOCABLE_IRQ == 20);
	assert(UK_INTCTLR_LAST_ALLOCABLE_IRQ == 207);
	assert(UK_INTCTLR_ALLOCABLE_IRQ_COUNT == 188);
	assert(uk_intctlr_msix_alloc(0, NULL, &addr, &data) == -EINVAL);
	assert(uk_intctlr_msix_alloc(0, &irq, NULL, &data) == -EINVAL);
	assert(uk_intctlr_msix_alloc(0, &irq, &addr, NULL) == -EINVAL);
	assert(uk_intctlr_msix_alloc(1, &irq, &addr, &data) == -ENOTSUP);
	assert(alloc_calls == 0);
	alloc_error = -ENOSPC;
	assert(uk_intctlr_msix_alloc(0, &irq, &addr, &data) == -ENOSPC);
	assert(irq == 999 && addr == 0x1234 && data == 0x5678);
	alloc_error = 0;
	assert(uk_intctlr_msix_alloc(0, &irq, &addr, &data) == 0);
	assert(irq == 20 && data == 52 && addr == 0xfee5a000ULL);
	assert(uk_intctlr_msix_free(irq) == 0);
	next_irq = UK_INTCTLR_LAST_ALLOCABLE_IRQ;
	assert(uk_intctlr_msix_alloc(0, &irq, &addr, &data) == 0);
	assert(irq == 207 && data == 239 && addr == 0xfee5a000ULL);
	assert(uk_intctlr_msix_free(irq) == 0);
	assert(uk_intctlr_msix_free(19) == -EINVAL);
	assert(uk_intctlr_msix_free(208) == -EINVAL);
	assert(uk_intctlr_msix_free(999) == -EINVAL);
	assert(free_calls == 2);
	puts("MANA MSI-X patch: PASS (mock IRQ allocator and CPUID)");
	return 0;
}
