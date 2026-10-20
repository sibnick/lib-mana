/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_PCI_IDS_H
#define MANA_PCI_IDS_H

#include <uk/bus/pci.h>

/*
 * PCI ids the MANA driver claims. The list holds exactly the ids that
 * include/uk/mana.h declares for the MVP. The pinned Unikraft core stops
 * its scan at the trailing all-ANY entry, so that entry is a required
 * terminator, not a match-all request.
 */
extern const struct pci_device_id mana_pci_ids[];

#endif
