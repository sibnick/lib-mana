/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <uk/alloc.h>
#include <uk/bus/pci.h>
#include <uk/intctlr.h>
#include <uk/intctlr/msix.h>
#include <uk/mana.h>
#include <uk/mana/gdma.h>
#include <uk/netdev_driver.h>

/* Keep references so the link checks the platform ABI without running it. */
static int (*volatile msix_alloc_api)(unsigned int, unsigned int *,
				     __u64 *, __u32 *) = uk_intctlr_msix_alloc;
static int (*volatile msix_free_api)(unsigned int) = uk_intctlr_msix_free;
static void (*volatile pci_register_api)(struct pci_driver *) =
	_pci_register_driver;

int main(int argc __unused, char *argv[] __unused)
{
	if (!msix_alloc_api || !msix_free_api || !pci_register_api)
		return 1;
	printf("MANA scaffold %04x:%04x, GDMA page %u, Unikraft %s\n",
	       mana_build_info.pci_vendor_id, mana_build_info.pci_device_id,
	       MANA_GDMA_PAGE_SIZE, mana_build_info.unikraft_revision);
	printf("Scaffold only: no PCI probe or network interface.\n");
	return 0;
}
