# MANA PCI and GDMA bootstrap

## Implementation boundary

The driver matches only Microsoft MANA VF `1414:00ba`.
It implements HWC bootstrap, not MANA port configuration or a network interface.
It uses one interrupt vector, one in-flight command, and six contiguous 4 KiB DMA pages.
Four pages hold EQ, CQ, RQ, and SQ rings.
Two pages hold the response and request messages.

The sequence checks PCI identity, BAR resources, VF registers, and MSI-X resources.
BAR mappings need page-aligned memory resources of at least 4 KiB.
It then allocates DMA pages and sends the shared-memory channel (SMC) establish request.
HWC init events provide queue IDs, a doorbell ID, a protection domain, and a GPA memory key.
The driver validates all required init fields before it posts a receive WQE.
It sends GDMA generate-test-EQE and requires a test event and interrupt delivery.
It verifies protocol version 1, lists GDMA devices, and queries resource limits.
Only these completed steps set `ready`.
No GDMA or MANA success is fabricated when a platform or protocol step fails.

The VF destination queue IDs remain zero, as in the Linux VF path.
The driver reports OS type `Other` and advertises no unsupported driver capabilities.
The [shared data queue layer](data-queues.md) registers separate data-queue memory regions after device control starts.
HWC uses the bootstrap pages and the GPA memory key from its init events.

## Platform contract

`LIBMANA` compiles the PCI driver and platform adapter.
`LIBMANA_DIRECT_DMA` is disabled by default.
Without it, probe returns `-ENOTSUP` before it changes PCI resources.
This is a fail-closed deployment gate.
Turning it on asserts the following facts.
It does not prove them.

| Required fact | Operator evidence |
|---|---|
| Device DMA addresses equal guest physical addresses | Record the hypervisor assignment and IOVA mapping contract for the VF. |
| No guest IOMMU translates the VF's addresses | Record the virtual hardware configuration and guest IOMMU exposure. |
| DMA RAM is shared and unencrypted | Exclude confidential/private-memory VM modes and record the guest memory policy. |
| PCI DMA observes coherent write-back RAM | Record the hypervisor and physical platform coherency guarantee. |
| MMIO has no conflicting write-back aliases | Record BAR mapping ownership and exclude other drivers or manual mappings. |

A host IOMMU can still enforce device isolation.
Its VF mapping must present the documented guest-physical address contract.
A missing virtual IOMMU alone does not prove that contract.
Neither `uk_palloc()`, a successful compile, nor this option proves device coherency.
If any fact is unknown, leave the option disabled.
Azure deployment evidence is still missing.

The adapter checks resident page translations, page alignment, write-back RAM PAT selection, and the uncached MMIO PAT entry.
It uses 64-bit DMA addresses, 64-bit doorbell writes, and explicit DMA barriers.
It retains driver state because this core has no PCI remove or driver-data callbacks.
It does not support automatic VF removal, recovery, or synthetic fallback.

## Interfaces and ownership

`include/uk/mana/platform.h` defines the production and host-test boundary.
Configuration callbacks use byte offsets and widths of 1, 2, or 4 bytes.
Mapping callbacks return a CPU address and the exact mapped size.
DMA callbacks return a separate device address and a zeroed, aligned, coherent allocation.
Each operation returns zero or a negative errno where its signature permits errors.
MMIO callbacks perform checked, aligned accesses within already validated mappings.
They cannot report recoverable PCI bus faults.
The host adapter aborts on an unexpected checked-MMIO failure rather than hiding it.

The caller supplies fresh, zeroed `struct mana_bootstrap` storage.
`mana_bootstrap_start()` owns successful mappings, IRQ allocation, and DMA pages.
The caller must retain that storage until `mana_bootstrap_stop()` succeeds.
The caller must also retain the platform callbacks and their context.
`mana_hwc_request()` accepts a 40-byte header with request versions 1 through 5 and response versions 1 through 3.
It validates the response header, command, device, activity ID, length, and completion queue.
It returns the actual response length separately.
Calls are serialized.
They are not safe from interrupt context.
The IRQ handler only records interrupt delivery.
`mana_bootstrap_poll()` processes EQ and CQ data outside interrupt context.
Later runtime users must call it to process asynchronous events.
The control API records source-defined link events through this poll.
See [Device and port control](control.md) for version handling and ownership rules.
Bootstrap stop returns `-EBUSY` until control releases device ownership.

`mana_unikraft_bootstrap()` returns driver-owned state, including quarantined state.
It returns NULL when no device remains claimed.
`mana_unikraft_stop()` shuts down the claimed device.
A Unikraft termination callback also calls this function.
No application or network interface may free driver-owned state.

SMC ownership waits have a 20-second bound.
Init-event waits have a 60-second bound.
Each command and test-event wait has a 30-second bound.
The adapter bounds its clock delay loop and returns an error if the clock stalls.
Each ring poll processes at most one ring.

A request timeout poisons the channel and retains its in-flight buffers.
Stop sends SMC destroy and waits for a valid response.
Only then does it disable bus mastering, stop MSI-X, release the IRQ, and free DMA pages.
A destroy timeout returns an explicit error and quarantines all resources.
It does not free pages that the device might still access.
A later stop can check the pending response after the device returns SMC ownership.
It does not replay a destroy command that already succeeded.
IRQ and unmap failures also retain resources until a successful retry.

## Focused host evidence

`tests/test_bootstrap.c` uses the real production PCI and GDMA units.
It uses the shared host allocator and the reusable synchronous model in `tests/host/bootstrap.{c,h}`.
`docs/host-tests.md` defines the model and request-response hook contracts.
It checks the SMC address packing and emitted RX/SQ WQE fields.
Synthetic device addresses differ from CPU pointers.
The test also uses nonzero high page-frame nibbles.

Coverage includes 32-bit and 64-bit BARs, MSI-X bounds and overlap, invalid resources, and missing DMA guarantees.
It injects each DMA allocation failure and each PCI configuration failure.
It rejects malformed init events, bad owners, wrong queue IDs, bad message IDs, invalid lengths, and firmware errors.
It exercises owner wrap across all three owner bits.
It checks missing interrupts, missing completions, stopped clocks, and all bounded timeout paths.
It checks DMA quarantine and cleanup retries after destroy, IRQ release, and unmap errors.
The actual patched PIC tests cover IRQs 15, 16, 20, and 207 under UBSan.
They preserve legacy PIC behavior and reject PIC accesses for MSI-X IRQs.

Run `make test`, `make test-sanitize`, and `make unikraft-test`.
The last command compiles the actual pinned adapter and MSI-X patch.
It does not attach or emulate physical MANA hardware.

## Authoritative source pin

Wire layouts and the HWC sequence follow Linux check-in
`0fcb05b72344f40806cbe810de8872f1698bf053`.
The source files use `GPL-2.0 OR BSD-3-Clause`.
This project uses the BSD-3-Clause option and retains the Microsoft notice.

- [GDMA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/gdma.h)
- [HWC definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/hw_channel.h)
- [GDMA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/gdma_main.c)
- [HWC implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/hw_channel.c)
- [SMC implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/shm_channel.c)

Unikraft remains pinned to `e31b2c44effe583eb8910e713fda814b6e049213`.
The local MSI-X patch adds legacy PIC range guards for its allocated IRQ pool.
It keeps the existing allocator and MSI message format.
The platform checks `plat/native/arch/x86_64/{paging.c,pt.c}` for the actual PAT mapping behavior.

## Remaining hardware gates

No available source proves the direct-DMA deployment contract for this Unikraft Azure image.
An operator must supply the deployment evidence before enabling the option.
An Azure MANA VM must then prove SMC responses, coherent queue writes, MSI-X delivery, and GDMA discovery.
These host tests cannot establish those facts.
The later Azure ticket must also validate direct VF port setup and packet traffic.
This implementation must not close the hardware gate or claim a working network driver.
