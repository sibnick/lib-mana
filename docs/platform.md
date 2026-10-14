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
The scaffold does not register a PCI driver or match devices.

## DMA APIs and limits

`lib/ukalloc/include/uk/alloc.h` provides `uk_palloc()` and `uk_pfree()`.
These allocate and release raw pages. They do not return a separate device DMA address.
`lib/ukpaging/include/uk/paging.h` provides `uk_paging_virt_to_phys()`.
`lib/ukvmem/include/uk/vmem/vma_types.h` provides `uk_vma_map_dma()`.
The latter maps an existing physical range. It does not allocate coherent DMA memory
or register a GDMA memory region.

The source audit found no Linux-style `dma_alloc_coherent()` or `dma_map_single()`
device DMA API in this base.
The bootstrap implementation must provide a checked platform DMA layer.
It must establish address translation, alignment, cache policy, device visibility,
memory ordering, and lifetime before it enables device DMA.
Do not cast a virtual pointer to a device address or treat `uk_palloc()` as proof of coherency.
Do not assume identity DMA under an IOMMU.
GDMA memory-region registration remains a separate protocol operation.

The GDMA public header records 4 KiB pages, 32-byte WQE units,
64-byte CQEs, and 16-byte EQEs.
These are protocol facts, not queried resource limits.
They do not define a C descriptor layout.

## Network APIs

`lib/uknetdev/include/uk/netdev_driver.h` exposes `uk_netdev_drv_register()`.
The library configuration selects `LIBUKNETDEV`, `LIBUKBUS_PCI`, and `LIBUKALLOC`.
No netdev callbacks or network interface exist in this scaffold.
The later integration ticket must implement queue and device lifecycle ownership.

## Validation boundary

Host checks prove that the scaffold headers and metadata compile and link.
The Unikraft check builds the actual library and local core patch.
Neither check proves PCI probe, coherent DMA, HWC startup, or Azure network traffic.
Those checks belong to the bootstrap and Azure validation tickets.

Reference: `../lib-ena/samples/httpreply-mc/patches/unikraft-e31b2c44.patch`.
Protocol sources and license findings are recorded in the Fossil research wiki.
