/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/test.h"
#include <stdio.h>
#include <string.h>
#include <uk/mana.h>
#include <uk/mana/gdma.h>

int main(void)
{
	TEST_CHECK(strcmp(mana_build_info.unikraft_revision,
		      "e31b2c44effe583eb8910e713fda814b6e049213") == 0);
	TEST_CHECK(mana_build_info.pci_vendor_id == 0x1414U);
	TEST_CHECK(mana_build_info.pci_device_id == 0x00baU);
	TEST_CHECK(mana_build_info.vport_count == 1U);
	TEST_CHECK(mana_build_info.queue_pairs == 1U);
	TEST_CHECK(mana_build_info.hwc_min_msix_vectors == 1U);
	TEST_CHECK(MANA_GDMA_PROTOCOL_VERSION == 1U);
	TEST_CHECK(MANA_GDMA_PAGE_SIZE == 4096U);
	TEST_CHECK(MANA_GDMA_WQE_UNIT_SIZE == 32U);
	TEST_CHECK(MANA_GDMA_CQE_SIZE == 64U);
	TEST_CHECK(MANA_GDMA_EQE_SIZE == 16U);
	puts("MANA scaffold: PASS (no device initialization)");
	return 0;
}
