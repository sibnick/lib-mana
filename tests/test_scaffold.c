/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uk/mana.h>
#include <uk/mana/gdma.h>

int main(void)
{
	assert(strcmp(mana_build_info.unikraft_revision,
		      "e31b2c44effe583eb8910e713fda814b6e049213") == 0);
	assert(mana_build_info.pci_vendor_id == 0x1414U);
	assert(mana_build_info.pci_device_id == 0x00baU);
	assert(mana_build_info.vport_count == 1U);
	assert(mana_build_info.queue_pairs == 1U);
	assert(mana_build_info.hwc_min_msix_vectors == 1U);
	assert(MANA_GDMA_PROTOCOL_VERSION == 1U);
	assert(MANA_GDMA_PAGE_SIZE == 4096U);
	assert(MANA_GDMA_WQE_UNIT_SIZE == 32U);
	assert(MANA_GDMA_CQE_SIZE == 64U);
	assert(MANA_GDMA_EQE_SIZE == 16U);
	puts("MANA scaffold: PASS (no device initialization)");
	return 0;
}
