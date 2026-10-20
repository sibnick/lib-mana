/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Test-only mirror of the pinned Unikraft PCI device id definitions.
 * Field names, field order, types, and macro values match
 * drivers/ukbus/pci/include/uk/bus/pci.h in Unikraft revision
 * e31b2c44effe583eb8910e713fda814b6e049213. Host suites compile the
 * production id table against this header. Production builds use the
 * real core header, never this file.
 */
#ifndef MANA_TEST_UK_BUS_PCI_H
#define MANA_TEST_UK_BUS_PCI_H

#include <stdint.h>

#define PCI_ANY_ID       (0xffff)
#define PCI_CLASS_ANY_ID (0xffffff)

struct pci_device_id {
	/**< Class ID or PCI_CLASS_ANY_ID. */
	uint32_t class_id;
	/**< Sub class ID or PCI_CLASS_ANY_ID. */
	uint32_t sub_class_id;
	/**< Vendor ID or PCI_ANY_ID. */
	uint16_t vendor_id;
	/**< Device ID or PCI_ANY_ID. */
	uint16_t device_id;
	/**< Subsystem vendor ID or PCI_ANY_ID. */
	uint16_t subsystem_vendor_id;
	/**< Subsystem device ID or PCI_ANY_ID. */
	uint16_t subsystem_device_id;
};

#define PCI_DEVICE_ID(vend, dev)           \
	.class_id = PCI_CLASS_ANY_ID,      \
	.sub_class_id = PCI_CLASS_ANY_ID,  \
	.vendor_id = (vend),               \
	.device_id = (dev),                \
	.subsystem_vendor_id = PCI_ANY_ID, \
	.subsystem_device_id = PCI_ANY_ID

#define PCI_ANY_DEVICE_ID                  \
	.class_id = PCI_CLASS_ANY_ID,      \
	.sub_class_id = PCI_CLASS_ANY_ID,  \
	.vendor_id = PCI_ANY_ID,           \
	.device_id = PCI_ANY_ID,           \
	.subsystem_vendor_id = PCI_ANY_ID, \
	.subsystem_device_id = PCI_ANY_ID

#endif
