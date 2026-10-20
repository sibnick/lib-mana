# MANA PCI and GDMA bootstrap

## Implementation boundary

The driver matches only Microsoft MANA VF `1414:00ba`.
The claimed-id table in `src/pci_ids.c` lists exactly the ids that `include/uk/mana.h` declares.
The pinned Unikraft core stops its scan at the trailing all-ANY entry.
That entry is a required list terminator.
It does not claim every PCI device.
This module implements HWC bootstrap.
The [netdev integration](netdev.md) starts control and registers the network interface after bootstrap succeeds.
It uses one interrupt vector, one in-flight command, and six contiguous 4 KiB DMA pages.
Four pages hold EQ, CQ, RQ, and SQ rings.
Two pages hold the response and request messages.

The sequence checks PCI identity, BAR resources, VF registers, and MSI-X resources.
The driver reads back and verifies command register and MSI-X control writes.
A readback mismatch fails with `-EIO`, like a silent write failure.
BAR mappings need page-aligned memory resources of at least 4 KiB.
It then allocates DMA pages and sends the shared-memory channel (SMC) establish request.
GDMA device addresses must stay below 2^52.
The SMC establish packs one page address as a 48-bit low frame field plus one 4-bit high nibble.
The driver rejects an allocated page at or above 2^52 with `-EPROTO`.
It rejects before enabling bus mastering and before writing the SMC message.
An unaligned page address fails the alignment gate first.
HWC init events provide queue IDs, a doorbell ID, a protection domain, and a GPA memory key.
The driver validates all required init fields before it posts a receive WQE.
It sends GDMA generate-test-EQE and requires a test event and interrupt delivery.
It verifies protocol version 1, lists GDMA devices, and queries resource limits.
Only these completed steps set `ready`.
No GDMA or MANA success is fabricated when a platform or protocol step fails.

The driver stores init fields 10 and 11, the PF destination RQ and CQ IDs.
Every SQ WQE sends both stored values in its destination fields.
The Linux pin stores the same fields and uses them when it builds each SQ WQE destination.
Current PF firmware is expected to report zero for both fields.
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
It validates the response command, version floor, message ID, device echo, activity echo, and length bounds.
It reads neither the response header type word nor the reserved word, like the pin.
It returns the actual response length separately.
Calls are serialized.
They are not safe from interrupt context.
The IRQ handler only records interrupt delivery.
`mana_bootstrap_poll()` processes EQ and CQ data outside interrupt context.
Later runtime users must call it to process asynchronous events.
The control API records source-defined link events through this poll.
The pinned EQ dispatcher ignores some event types.
This library ignores them too and counts them in `ignored_events`.
See [Firmware tolerance](firmware-tolerance.md) for the per-case evidence.
See [Device and port control](control.md) for version handling and ownership rules.
Bootstrap stop returns `-EBUSY` until control releases device ownership.

`mana_unikraft_bootstrap()` returns driver-owned state, including quarantined state.
It returns NULL when no device remains claimed.
`mana_unikraft_stop()` stops RX, TX, shared data queues, control, and then bootstrap.
It stops at the first teardown error and retains remaining resources.
A Unikraft termination callback also calls this function.
No application or network interface may free driver-owned state.

SMC ownership waits have a 20-second bound.
Init-event waits have a 60-second bound.
Each command and test-event wait has a 30-second bound.
The adapter bounds its clock delay loop and returns an error if the clock stalls.
Each ring poll processes at most one ring.

A request timeout poisons the channel and retains its in-flight buffers.
Stop sends SMC destroy only after a validated establish.
The pin applies the same rule with its `max_num_cqs > 0` guard.
If the establish failed or never validated, stop sends no destroy request.
It then clears bus mastering and releases resources as usual.
After a validated establish, stop waits for a valid destroy response.
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
The test allocates the six ring pages so the last lands 4 KiB below 2^52.
It rejects every page position whose device address is exactly 2^52.
The model sends nonzero PF destination IDs in init fields 10 and 11.
It checks both values in the destination of every SQ WQE.

Coverage includes 32-bit and 64-bit BARs, MSI-X bounds and overlap, invalid resources, and missing DMA guarantees.
It applies the pinned core match rule to the id table: the VF id matches and non-MANA ids do not.
It aborts the start and quarantines a failed stop when the model drops MSI-X control writes.
It injects each DMA allocation failure and each PCI configuration failure.
It rejects malformed init events, bad owners, wrong queue IDs, bad message IDs, invalid lengths, and firmware errors.
It ignores tolerated EQ event types during establish and counts them in `ignored_events`.
It sends SMC destroy only after a validated establish and counts zero destroys for establish failures.
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
