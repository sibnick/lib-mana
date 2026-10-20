# MANA transmit datapath

## Scope and source

Ticket `6308ec4803` adds one basic packet TX queue.
It uses the shared queue API for registration, rings, doorbells, CQ owners, and object destruction.
It does not add offloads, RSS, multiple SGEs, or a network interface.
The direct-DMA deployment gate remains disabled by default.
No Azure device has tested this packet path.

Wire fields and FIFO handling follow Linux check-in `0fcb05b72344f40806cbe810de8872f1698bf053`.
The implementation uses the BSD-3-Clause option and retains the Microsoft notice.
The source files are:

- [MANA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/mana.h)
- [MANA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/mana_en.c)

See [Shared data queues](data-queues.md), [Control](control.md), and [Bootstrap](bootstrap.md) for the lower-layer contracts.
Successful queue creation consumes its region handles.
TX never destroys those handles separately.

## API and packet ownership

Use `include/uk/mana/tx.h`.
Supply fresh, zeroed TX storage and a separate record array.
Retain the TX storage, record array, shared data context, control, bootstrap, and platform callbacks until stop succeeds.
Serialize all calls outside interrupt context.
Do not change the embedded queue or records while TX owns them.
Do not reenter these APIs from the stop callback.

`mana_tx_start` creates one embedded shared TX queue.
The record capacity cannot exceed either the WQ basic-unit count or the CQ entry count.
The shared layer checks ring sizes and device queue resources.
A failed start can still own queue resources.
Call `mana_tx_stop` to release them unless the context has entered quarantine.

`mana_tx_submit(tx, frame, length, cookie)` accepts one linear Ethernet frame.
It returns 1 only after the shared layer publishes the WQE.
A negative errno leaves cookie ownership with the caller.
The caller owns the original frame bytes throughout.
Submit borrows those bytes only during the call and copies them into coherent DMA.
After the call, the caller can change or release the original bytes.
The cookie identifies the accepted packet without requiring a network-interface type.
The caller must not reuse an accepted cookie until poll or the stop callback returns it.
Null cookies are valid.

Each accepted packet owns one contiguous coherent DMA allocation and one FIFO record.
The allocation rounds up to 4 KiB.
The frame length must include the Ethernet header and fit the configured MTU plus 14 bytes.
Frames shorter than 60 bytes receive zero padding.
The result preserves both the original length and the wire length.
All remaining allocation bytes also contain zero.
No caller pointer or CPU address becomes an SGE device address.
The SGE uses the platform device address and the registered MANA memory key.

Full record storage or a full WQ returns `-ENOSPC` before allocation or publication.
Allocation failure returns its platform error without taking cookie ownership.
A rejected DMA contract frees only the unpublished allocation.
A shared post failure also frees only the unpublished allocation.
Existing accepted packets remain owned.

`mana_tx_poll(tx, result)` returns 1 for one completed packet, 0 for empty, or a negative queue error.
One result returns its cookie exactly once and releases the corresponding DMA allocation.
`result.error` reports packet failure separately from the poll return value.
Known TX drop types or a nonzero vendor status set `result.error` to `-EIO`.
An unknown completion type also sets `-EIO`.
The pinned poll logs the unknown type and still frees the packet (`mana_en.c:2002-2011`).
The result preserves the source-defined type and 24-bit vendor status.
Packet failure also calls the platform error callback.
It does not by itself poison the queue.

## Wire fields and completion checks

The basic packet uses one SGE and client data unit zero.
All checksum, segmentation, VLAN-insertion, encapsulation, and completion-suppression flags remain zero.
The virtual SQ frame field remains zero, as in the pinned consumer's basic TX queue.

| Field | Encoding |
|---|---|
| Packet format | Short 0, long 1 |
| Completion queue | 24-bit CQ ID in OOB word zero, bits 8 through 31 |
| Short virtual-port offset | OOB word one, bits 24 through 31 |
| Long virtual-port offset | OOB word three, bits 16 through 27 |
| Inline OOB length | 8 bytes short, 24 bytes long |
| SGE | One device address, registered memory key, and padded frame length |

TX uses short OOB only when control permits it and the virtual-port offset fits eight bits.
Other packets use long OOB with a 12-bit virtual-port offset.
An offset outside that range fails start.
The shared layer writes the generic GDMA header and pads the WQE.
Short WQEs use one 32-byte basic unit.
Long WQEs use two basic units.

Poll services control and data EQ events before reading the TX CQ.
It polls the CQ independently of notifications.
The shared layer checks CQ owner bits, WQ identity, SQ direction, GDMA reserved bits, and MANA client type.
The pinned poll logs an unknown TX completion type and still frees the packet (`mana_en.c:2002-2011`).
This layer completes such a record with `result.error` set to `-EIO`.
Wrong identity, client type, or a malformed owner enters quarantine without retiring the packet.

Linux `mana_poll_tx_cq` uses FIFO order to match each completion to its packet.
It does not interpret `tx_data_offset`, `tx_sgl_offset`, or `tx_wqe_offset`.
The definitions provide field widths but do not prove offset units or zero values for basic packets.
TX therefore returns these decoded fields as diagnostics without enforcing guessed values.
It also ignores the twelve reserved client words, as the pinned consumer does.
The shared layer still validates the GDMA completion information word.
Hardware validation must record these packet fields before any stricter rule can use them.

TX validates the oldest local record before retirement.
The record must own DMA, fit its allocation, and contain the expected complete WQE size.
Its posted byte offset must equal the shared WQ tail byte offset.
TX passes that exact local record to `mana_data_retire`.
It does not infer a retirement offset from unproven device fields.
FIFO, WQ, and record corruption enter quarantine.

## Shutdown and quarantine

`mana_tx_stop` requires a completion callback while accepted packets remain.
Without that callback, it returns `-EINVAL` without stopping submissions.
Stop then prevents submissions and quiesces the shared queue.
Each successful drain poll returns a packet result through the callback.
The callback receives packet failures as well as successes.
Packet failures do not change the stop return value.

Drain permits at most 10000 empty polls with 1 ms delays.
Successful polls strictly reduce the pending FIFO count.
Thus total drain polls cannot exceed the initial pending count plus 10000.
Stop checks for an extra completion and requires the WQ head to equal its tail.
Only then does it destroy the queue through the shared layer.
Successful stop clears TX storage and ends record-array ownership.
The shared data context remains with its caller for RX or later teardown.

A valid firmware destroy failure retains the stopped object and permits a stop retry.
A transport failure, malformed completion, drain timeout, or clock failure enters quarantine.
Quarantine retains all pending packet DMA, cookies, and queue memory.
Results already returned before a later failure remain completed.
No TX API frees uncertain DMA or fabricates a cancellation result.
An operator must prove device DMA has stopped before any external release.
HWC teardown alone does not supply that proof.

## Evidence and remaining gates

`tests/test_tx.c` links the production TX, shared queue, control, PCI, and GDMA sources.
Its packet and data-command model stays local to that test.
The reusable bootstrap response hook transports its source-defined responses.
Synthetic packet device addresses differ from CPU pointers.

The tests check complete short and long WQE bytes, padding, MTU bounds, multi-page DMA, and consumed region handles.
They cover record and WQ exhaustion, FIFO wrap, all CQ owner bits, and 32-bit WQ counter wrap.
They also cover packet statuses, diagnostic fields, malformed completions, corrupted records, allocation failures, and shutdown ownership.
Normal, sanitizer, and pinned Unikraft checks exercise this source path.
The Unikraft link check references the exported packet API.

These checks do not prove Azure DMA coherency, queue acceptance, TX completion field behavior, interrupts, or packet traffic.
The deployment evidence in [Bootstrap](bootstrap.md) remains mandatory.
No test enables direct DMA, creates a network interface, or closes the hardware gate.
