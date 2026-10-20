/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * PCI device id table for the MANA driver. Keep this list in sync with
 * the ids declared in include/uk/mana.h. The trailing all-ANY entry
 * terminates the list for the pinned Unikraft PCI core. It is required
 * by that core and it stops the scan. It does not claim any device.
 */
#include "pci_ids.h"

#include <uk/mana.h>

const struct pci_device_id mana_pci_ids[] = {
	{ PCI_DEVICE_ID(MANA_PCI_VENDOR_ID, MANA_PCI_VF_DEVICE_ID) },
	{ PCI_ANY_DEVICE_ID },
};
