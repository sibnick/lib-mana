/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/mana.h>

const struct mana_build_info mana_build_info = {
	.unikraft_revision = MANA_UNIKRAFT_REVISION,
	.pci_vendor_id = MANA_PCI_VENDOR_ID,
	.pci_device_id = MANA_PCI_VF_DEVICE_ID,
	.vport_count = MANA_MVP_VPORT_COUNT,
	.queue_pairs = MANA_MVP_QUEUE_PAIRS,
	.hwc_min_msix_vectors = MANA_HWC_MIN_MSIX_VECTORS,
};
