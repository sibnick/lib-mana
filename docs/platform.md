# Pinned Unikraft platform

## Target and local patch

Use Unikraft `e31b2c44effe583eb8910e713fda814b6e049213`.
Its `version.mk` reports 0.21.0.
The first build target is a single-core x86_64 KVM guest.
`LIBMANA` is disabled by default and requires this target.

The base lacks `uk/intctlr/msix.h` and `uk_intctlr_msix_alloc/free`.
The local patch adds these APIs to xPIC when `LIBUKINTCTLR_APIC` is enabled.
It follows the MSI-X parts of the adjacent lib-ena multi-core patch.
It does not include that sample's timer, scheduler, ACPI, or application changes.
It also stops MSI-X IRQs from reaching legacy PIC shifts or port accesses.
The PIC guard preserves IRQs 0 through 15.
The adapter checks CPUID and live x2APIC enable bits before it allocates an IRQ.
It also checks that the live APIC ID fits the allocator's 8-bit destination format.
This check catches the pinned core's ignored APIC enable errors.

The allocator targets CPU zero and reads the executing BSP's initial APIC ID
with CPUID. It puts that ID in MSI address bits 19:12.
It does not guess the APIC ID from a CPU index.
The IRQ pool uses 20 through 207, or vectors 52 through 239.
This leaves the timer, IPI bands, and spurious vector outside the pool.
The compile check rejects SMP configurations.

This API only allocates a vector and builds its message.
The driver must register a handler with `uk_intctlr_irq_register()`.
It must validate, mask, program, and unmask the PCI MSI-X table separately.
It must stop device interrupts and unregister the handler before freeing an IRQ.
Vector allocation is not proof that Azure delivers an HWC interrupt.
The guarded patch SHA-256 is
`34e0fc1c7a9af4cc4ccde793cae784eee91fe1121e4e3b15aac873da167aa6a6`.
The preparation script can upgrade the original, stamped MSI-X patch.
It checks the original patch prefix and pristine PIC checksum before the upgrade.
It rejects unknown patch or PIC changes instead of overwriting them.

## PCI APIs

The checked base defines these interfaces in
`drivers/ukbus/pci/include/uk/bus/pci.h`:

- `struct pci_driver` has `init` and `add_dev` callbacks.
- `PCI_REGISTER_DRIVER()` registers a driver through a constructor.
- `struct pci_device` exposes the identity, PCI address, driver, base, and IRQ.

It has no remove callback or per-device driver-data member.
Keep MANA state in driver-owned storage.
Do not claim support for automatic VF removal and restoration.
The bootstrap ticket must check BAR bounds, PCI command bits, and device resources.
The driver matches only VF `1414:00ba`.
It supports one device in driver-owned storage.
It rejects an existing bus-master owner and an enabled MSI-X capability.
It checks BAR sizes with memory decoding disabled and restores each BAR after sizing.
It uses width-specific configuration writes, which avoid PCI status write-one-to-clear bits.

## DMA APIs and limits

`lib/ukalloc/include/uk/alloc.h` provides `uk_palloc()` and `uk_pfree()`.
These allocate and release raw pages. They do not return a separate device DMA address.
`lib/ukpaging/include/uk/paging.h` provides `uk_paging_virt_to_phys()`.
`lib/ukvmem/include/uk/vmem/vma_types.h` provides `uk_vma_map_dma()`.
The latter maps an existing physical range. It does not allocate coherent DMA memory
or register a GDMA memory region.

The source audit found no Linux-style `dma_alloc_coherent()` or `dma_map_single()`
device DMA API in this base.
The platform layer requires `LIBMANA_DIRECT_DMA` before it can enable DMA.
This disabled-by-default option asserts a deployment contract, not a discovered hardware fact.
The contract requires coherent, unencrypted guest RAM and identity device DMA addresses.
Do not enable it under a translating guest IOMMU or a confidential VM.
The hypervisor must map device addresses to the corresponding guest physical RAM.

The adapter allocates one raw page per HWC ring or message buffer.
It also allocates contiguous multi-page memory for the shared data queue layer.
It populates every page before it checks its address.
It checks each page's alignment, contiguous physical translation, and write-back PAT selection.
It never casts a virtual pointer to a device address.
The address translation does not prove the hypervisor's device mapping.
See [Bootstrap](bootstrap.md) for the operator checks and missing Azure evidence.

MMIO mappings use `uk_vma_map_dma()` with populated 4 KiB pages.
The pinned `UK_ARCH_X86_64_PAGE_ATTR_WRITECOMBINE` name is misleading.
Its implementation sets PCD and PWT, which select PAT index 3.
The pinned platform programs index 3 as uncached, not write-combining.
The adapter checks the live PAT before it creates each mapping.
It uses aligned volatile accesses and 64-bit doorbell writes.
DMA read and write barriers use LFENCE and SFENCE with compiler memory barriers.
GDMA memory-region registration remains a separate protocol operation.
The [shared data queue layer](data-queues.md) implements this operation after control registers the MANA device.
HWC uses its SMC bootstrap page addresses and firmware-supplied GPA memory key.
It does not need the later GDMA data-queue memory-region registration.

The GDMA public header records 4 KiB pages, 32-byte WQE units,
64-byte CQEs, and 16-byte EQEs.
These are protocol facts, not queried resource limits.
They do not define a C descriptor layout.

## Network APIs

`lib/uknetdev/include/uk/netdev_driver.h` exposes `uk_netdev_drv_register()`.
The library configuration selects `LIBUKNETDEV`, `LIBUKBUS_PCI`, and `LIBUKALLOC`.
No netdev callbacks or network interface exist.
The later integration ticket must implement queue and device lifecycle ownership.

## Validation boundary

Host checks exercise the real PCI and GDMA sources against a focused HWC device model.
They check requests, init events, completions, owner wrap, bounds, failures, and resource ownership.
The Unikraft check builds the actual adapter, protocol units, PCI registration, and local core patch.
Neither check proves physical PCI access, hypervisor DMA mappings, Azure interrupts, or Azure network traffic.
The Azure validation ticket must prove these hardware facts.

Reference: `../lib-ena/samples/httpreply-mc/patches/unikraft-e31b2c44.patch`.
Protocol sources and license findings are recorded in the Fossil research wiki.
