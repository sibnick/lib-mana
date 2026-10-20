/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <uk/alloc.h>
#include <uk/bus/pci.h>
#include <uk/intctlr.h>
#include <uk/intctlr/msix.h>
#include <uk/mana.h>
#include <uk/mana/gdma.h>
#include <uk/mana/bootstrap.h>
#include <uk/mana/control.h>
#include <uk/mana/data_queue.h>
#include <uk/mana/tx.h>
#include <uk/mana/rx.h>
#include <uk/mana/netdev.h>
#include <uk/netdev_driver.h>

/* Keep references so the link checks the platform ABI without running it. */
static int (*volatile msix_alloc_api)(unsigned int, unsigned int *,
				     __u64 *, __u32 *) = uk_intctlr_msix_alloc;
static int (*volatile msix_free_api)(unsigned int) = uk_intctlr_msix_free;
static void (*volatile pci_register_api)(struct pci_driver *) =
	_pci_register_driver;
static struct mana_bootstrap *(*volatile bootstrap_api)(void) =
	mana_unikraft_bootstrap;
static int (*volatile stop_api)(void) = mana_unikraft_stop;
static int (*volatile control_api)(struct mana_control *, struct mana_bootstrap *,
				  uint32_t) = mana_control_start;
static int (*volatile queue_api)(struct mana_control *,
				struct mana_control_queue *,
				enum mana_control_queue_type,
				const struct mana_control_queue_spec *) =
	mana_control_create_queue;
static int (*volatile data_api)(struct mana_data *, struct mana_control *,
			       uint32_t) = mana_data_start;
static int (*volatile tx_api)(struct mana_tx *, const void *, uint32_t, void *) =
	mana_tx_submit;
static uint32_t (*volatile tx_free_api)(const struct mana_tx *) =
	mana_tx_free_packets;
static int (*volatile rx_api)(struct mana_rx *, struct mana_rx_packet *) =
	mana_rx_poll;
static int (*volatile netdev_api)(struct mana_netdev *, struct mana_bootstrap *,
				 struct uk_alloc *) = mana_netdev_register;
static int (*volatile netdev_stop_api)(struct mana_netdev *) = mana_netdev_stop;

int main(int argc __unused, char *argv[] __unused)
{
	if (!msix_alloc_api || !msix_free_api || !pci_register_api ||
	    !bootstrap_api || !stop_api || !control_api || !queue_api || !data_api ||
	    !tx_api || !tx_free_api || !rx_api || !netdev_api || !netdev_stop_api)
		return 1;
	printf("MANA bootstrap %04x:%04x, GDMA page %u, Unikraft %s\n",
	       mana_build_info.pci_vendor_id, mana_build_info.pci_device_id,
	       MANA_GDMA_PAGE_SIZE, mana_build_info.unikraft_revision);
	printf("MANA uknetdev needs a checked direct DMA deployment contract.\n");
	return 0;
}
