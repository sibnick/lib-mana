/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_MANA_H
#define UK_MANA_H

#define MANA_PCI_VENDOR_ID 0x1414U
#define MANA_PCI_VF_DEVICE_ID 0x00baU
#define MANA_MVP_VPORT_COUNT 1U
#define MANA_MVP_QUEUE_PAIRS 1U
#define MANA_HWC_MIN_MSIX_VECTORS 1U
#define MANA_UNIKRAFT_REVISION "e31b2c44effe583eb8910e713fda814b6e049213"

#ifdef __cplusplus
extern "C" {
#endif

struct mana_build_info {
	const char *unikraft_revision;
	unsigned int pci_vendor_id;
	unsigned int pci_device_id;
	unsigned int vport_count;
	unsigned int queue_pairs;
	unsigned int hwc_min_msix_vectors;
};

/* Build scope only. Device capabilities must come from GDMA queries. */
extern const struct mana_build_info mana_build_info;

#ifdef __cplusplus
}
#endif

#endif
