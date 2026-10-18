# Shared GDMA data queues

## Scope and source

This slice supplies the shared queue prerequisite for TX `6308ec4803` and RX `acca155096`.
It creates queue memory, a separate data EQ, and one TX/RX WQ/CQ pair.
It does not implement MANA packet OOB fields, packet buffer management, or a network interface.
The direct-DMA deployment gate remains disabled by default.
No Azure device has tested these queues.

Wire formats follow Linux check-in `0fcb05b72344f40806cbe810de8872f1698bf053`.
The implementation uses the BSD-3-Clause option and retains the Microsoft notice.
The checked files are:

- [GDMA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/gdma.h)
- [GDMA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/gdma_main.c)
- [MANA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/mana.h)
- [MANA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/mana_en.c)

| Command | Code | Request bytes | Response bytes | Device |
|---|---|---|---|---|
| Create DMA region | 25 | 64 + 8 * page count | 40 | None, zero |
| Destroy DMA region | 27 | 48 | 32 | None, zero |
| Create EQ | 12 | 96 | 36 | Registered MANA device |
| Disable EQ | 13 | 52 | 32 | Registered MANA device |
| Generate test EQE | 10 | 44 | 32 | Registered MANA device |

These GDMA commands use request and response version 1.
The shared layer uses the existing control API for MANA queue object commands.
See [Control](control.md) for their lengths, versions, and ownership rules.

The region request specifies length, zero page offset, page type zero, and every 4 KiB device page address.
It does not contain a PD field.
EQ creation and MANA queue object creation connect the region to the registered device's PD.
The SQ/RQ GPA memory key comes from device registration, not HWC initialization.
The pin defines `GDMA_INVALID_DMA_REGION` as zero.
MANA object handles use a different invalid value, `UINT64_MAX`.

## Public API and ownership

Use `include/uk/mana/data_queue.h`.
Supply fresh, zeroed `mana_data` and `mana_data_queue` storage.
Retain the control, bootstrap, platform callbacks, and storage until teardown succeeds.
Serialize data, control, bootstrap, and packet operations outside IRQ context.
Do not destroy a shared layer's queue through the control API directly.

`mana_data_start(d, c, eq_size)` allocates and registers a separate EQ.
It uses MSI-X table index zero, which already owns the bootstrap IRQ.
The IRQ callback only records delivery.
The caller must poll both the control and data EQs.
Start sends a test event and requires its data EQE and interrupt delivery.
The wait has a 30-second bound.

`mana_data_queue_create(d, q, type, wq_size, cq_size)` allocates and registers both rings.
It sends `CREATE_WQ_OBJ` through the control API.
Success consumes both region handles.
The layer clears `region.handle` and records `region.consumed`.
The `original_handle` field records the consumed handle for diagnostics and duplicate detection only.
The layer never destroys a consumed region separately.
The queue's `object` field exposes its control object for steering and RX fencing.

A failed start or create can still own memory or a registered region.
For a proven firmware failure, call `mana_data_stop` or `mana_data_queue_destroy` to release those resources.
A destroy failure with a valid firmware response retains ownership and permits a retry.
A transport timeout, malformed success response, or invalid identifier quarantines the data context and control.
Unknown create results retain region handles and backing memory.
Unknown destroy results also retain backing memory.
No release API bypasses quarantine.
HWC destruction alone does not prove that data DMA has stopped.

`mana_data.regions` counts live backing allocations, including consumed and quarantined regions.
The data context owns one EQ and at most two WQ/CQ pairs.
The layer checks queried EQ, SQ, RQ, CQ, MSI-X, doorbell, and port queue counts.
The HWC queues reserve one resource in each global queue count.
Returned IDs must fit their wire fields.
Control also checks CQ table bounds and collisions with HWC and the other data object.
Control stop returns `-EBUSY` until the data context releases its ownership.

## Ring size and memory rules

Ring sizes must be power-of-two multiples of 4 KiB.
The complete page list must fit the queried HWC request limit.
The current 4096-byte HWC limit permits at most 504 listed pages.
Thus the largest accepted power-of-two ring is 1 MiB.
The layer does not implement `DMA_REGION_ADD_PAGES` or scattered allocations.
Resource counts do not imply a maximum ring byte size.
The layer does not invent a device size capability.
Firmware can reject an otherwise valid ring size.

The platform allocator returns zeroed, coherent, contiguous, aligned DMA memory.
The pinned Unikraft adapter supports multiple contiguous pages.
It checks every page's translation, endpoint, residency, and write-back PAT selection.
It frees the exact allocated page count.
These checks do not prove hypervisor DMA mappings, shared memory, or device coherency.
The deployment evidence in [Bootstrap](bootstrap.md) remains mandatory.

## Polling and doorbells

`mana_data_poll` processes at most one EQ ring.
Completion events must identify a live data CQ.
They set `queue.notified` without processing packet completions.
An event with an event type that the pinned GDMA header does not define is ignored.
The layer counts it in `data.unknown_events`, advances the EQ head, and keeps polling.
This matches the silent default case of `mana_gd_process_eqe` in the pinned source.
The pinned adapter EQ sets no service callback, so the pin also drops such events there.
A benign or newer firmware event therefore cannot poison the shared context.
Stop and shutdown stay recoverable.
Event types 129 through 135 and 176 are defined in the pin for device service or failure handling.
This library has no service or reset handler, so those types keep the quarantine path.
That keeps their intended fatal semantics.
Invalid CQ IDs, reserved bits, and invalid owners still quarantine the context.
The layer processes data events separately from the HWC CQ.

`mana_data_poll_cq(q, completion)` returns 1, 0, or a negative errno.
One means one copied completion.
Zero means the next entry still has the old owner.
The completion contains `wq_id`, `is_sq`, `kind`, and all 60 client bytes.
The layer checks the WQ ID, SQ/RQ direction, reserved bits, and MANA client type.
Packet-specific completion types, offsets, lengths, errors, and buffer bounds remain the packet worker's responsibility.
An empty CQ clears `queue.notified`.
Workers must poll live CQs independently rather than rely only on notifications.

EQ entries contain 12 detail bytes and a four-byte owner/type word.
CQ entries contain 60 client bytes and a four-byte owner/WQ word.
Both rings use all three owner bits.
The initial consumer head equals the number of entries.
The layer checks owner bits before the DMA read barrier and before reading payload bytes.
An unexpected owner returns `-EOVERFLOW` and retains the rings.

Doorbells use the registered MANA doorbell page.
SQ, RQ, CQ, and EQ offsets are `0`, `0x400`, `0x800`, and `0xff8`.
The layer uses 64-bit writes after the DMA write barrier.
WQ tail pointers contain the producer basic-unit count multiplied by 32.
RQ `wqe_cnt` remains zero, as the Linux source requires.
CQ and EQ tail pointers wrap across eight ring lengths and set the arm bit.
Each CQ call reads at most one entry and rearms after consumption.

## WQ submission and packet worker boundary

`mana_data_post` encodes the generic GDMA header, client OOB bytes, and SGEs.
SQ OOB size must be 8 or 24 bytes.
RQ uses an eight-byte zero inline area and accepts no client OOB.
SQ data units must fit 14 bits.
RQ data units must be zero.
The layer does not support OOB-in-SGL, padding flags, or chaining.

Each SGE contains a device address, the registered GPA memory key, and a nonzero byte length.
The layer rejects address overflow and any key other than the registered MANA GPA memory key.
The caller must supply memory that its platform owns and permits the device to access.
The layer cannot prove arbitrary SGE ownership from an address and key alone.
It neither allocates nor frees packet buffers.

WQEs use 32-byte basic units.
SQ and RQ WQEs cannot exceed 512 and 256 bytes.
The layer pads unused bytes with zero, copies across ring wrap, and checks available capacity before publication.
It returns the ring byte offset and basic-unit count.
A full WQ returns `-ENOSPC` without a doorbell or head change.

The packet worker keeps a FIFO of these posted records.
It validates the source-defined packet CQ offset and payload before retirement.
`mana_data_retire` accepts only the current tail offset and a bounded outstanding basic-unit count.
The worker must pass the exact complete oldest WQE record, not a partial record.
The shared layer does not infer packet completion offsets or retire WQEs from a CQ notification.
TX and RX workers must retain packet memory after an uncertain operation.

## Safe shutdown

For TX, stop new packet submissions.
Call `mana_data_queue_quiesce` to prevent more WQ posts.
Poll and validate TX completions.
Retire every outstanding TX WQE.
Call `mana_data_queue_destroy` only after the WQ drains.
The layer rejects TX destroy while its head differs from its tail.
After object destroy, the layer flushes pending EQ notifications with a test event before removing the CQ identity.
It accepts stale notifications only for that proven-destroyed object during this flush.

For RX, disable steering with `mana_control_set_rx(c, 0)`.
Quiesce the queue.
Send `mana_control_fence_rx(&q.object)`.
Continue polling and processing packet completions.
The HWC fence acknowledgment does not complete the fence.
A valid fence CQE must have the RX WQ ID, RQ direction, client type 1, type 3, and zero vendor error.
Only that CQE calls `mana_control_fence_complete` and returns `MANA_DATA_RX_FENCE`.
An unsolicited or malformed fence quarantines the context.

Use the existing bounded `mana_control_wait_fence` with a nonblocking CQ service callback.
The callback must process ordinary RX completions as well as the fence.
After the fence, destroy the RX object through `mana_data_queue_destroy`.
The layer releases outstanding ring storage only after the object destroy succeeds.
The RX worker still owns its packet buffer release rules.

After both data queues end, call `mana_data_stop`.
It flushes the EQ with a test event and disables the EQ before freeing its consumed backing memory.
Then stop control and bootstrap.
Retain all storage and packet memory after quarantine.
An operator must prove that the device cannot access this memory before any external release.

## Evidence and remaining gates

`tests/test_data_queue.c` links the real data queue, control, PCI, and GDMA production sources.
Its deterministic model checks exact GDMA request bytes and device IDs.
It checks multi-page lists, consumed handles, separate data doorbells, ring wrap, all owner bits, and typed completions.
It covers every ring allocation failure, malformed events, invalid IDs, bounds, full WQs, and invalid DMA.
It also covers firmware retry, uncertain registration/create/destroy results, fence validation, and EQ timeout bounds.
It proves the layer ignores and counts undefined EQ event types, keeps polling, and keeps teardown clean.
It also proves pinned service and failure event types still quarantine the context.
The existing host and control suites remain in the test targets.

Run `make test`, `make test-sanitize`, and `make unikraft-test`.
The last check compiles the actual adapter and links the shared queue API against the pinned core.
These checks do not prove DMA coherency, data EQ interrupts, Azure queue acceptance, or packet traffic.
TX/RX workers can now implement packet OOB, buffer ownership, CQ payload checks, and packet completion retirement.
The Azure hardware gates, default-disabled DMA setting, and lack of VF removal recovery remain unchanged.
