# MANA receive datapath

## Scope and source

Ticket `acca155096` adds basic Ethernet receive operations for one queue.
It uses the shared data queue, control, bootstrap, and platform APIs.
It adds no DMA region commands, rings, doorbells, owner protocol, or network interface.
The default-off `LIBMANA_DIRECT_DMA` gate remains unchanged.
RSS, packet offloads, coalesced completions, and multi-SGE packets remain unsupported.
No Azure device has tested this code.

Wire fields follow Linux check-in `0fcb05b72344f40806cbe810de8872f1698bf053`.
The code uses the BSD-3-Clause option and keeps the Microsoft copyright notice.
The source references are:

- [MANA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/mana.h)
- [MANA Ethernet implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/mana_en.c)
- [GDMA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/gdma_main.c)

`mana_alloc_rx_wqe` supplies one SGE, no inline client OOB, no flags, and a zero client data unit.
`mana_post_pkt_rxq` expects one 32-byte basic unit per WQE.
`mana_process_rx_cqe` retires buffers in posted FIFO order.
It handles ordinary packets and truncated packets differently.
Queue creation disables completion coalescing through the existing control API.
CQ capacity reserves one extra entry for the RX object fence.

## Public API

Use `include/uk/mana/rx.h`.
Start control and shared data queues first.
Supply fresh, zeroed `mana_rx` storage and a zeroed `mana_rx_slot` array.
Retain both until `mana_rx_stop` succeeds.
Serialize RX operations with data, control, and bootstrap operations outside interrupt context.
Do not access the embedded shared queue directly during normal RX operation.

`mana_rx_start(rx, data, slots, capacity, wq_size, cq_size)` creates the RX queue.
It checks that the WQ holds all buffers and the CQ holds all buffers plus the fence.
The shared layer checks ring sizes and queried resource limits.
Start allocates and posts every buffer before it enables RX steering.
Each buffer holds the local MTU plus the 14-byte Ethernet header.
The allocator rounds this size to complete 4 KiB pages.
The SGE exposes only the frame capacity, not the unused allocation bytes.
The SGE uses the registered MANA GPA memory key.
Packet headroom and delivery offset are zero.

Failed start can retain queue resources and posted buffers.
Call `mana_rx_stop` after a failed start unless RX is quarantined.
Do not overwrite or reuse the RX storage before successful stop.
The shared layer owns region registration and consumed region handles.
Successful object creation consumes both handles and clears them to zero.
RX never destroys a consumed region separately.

`mana_rx_poll(rx, packet)` processes one data CQ entry.
It polls the data EQ independently of CQ notifications.
The packet output must own no allocation.
The result makes ownership explicit:

| Result | Meaning | Packet ownership |
|---|---|---|
| `MANA_RX_EMPTY` | No new CQ entry | No transfer |
| `MANA_RX_PACKET` | One valid Ethernet packet | Caller owns the allocation |
| `MANA_RX_DROPPED` | Truncation, invalid frame length, stripped VLAN, or shutdown discard | RX frees the completed allocation |
| `MANA_RX_FENCE` | Shared layer validated the RX fence | No packet transfer |
| Negative errno | Invalid call or uncertain queue state | No transfer |

The packet contains the allocation, offset, length, saved release function, and saved allocator context.
Packet bytes start at `packet.dma.addr + packet.offset`.
Do not copy packet ownership or free its memory through another allocator.
Call `mana_rx_packet_release` exactly once.
The release function clears the packet after it frees the allocation.
This contract permits later `uknetdev` integration without tying a delivered packet to an RX slot.

Call `mana_rx_refill` after packet delivery or a drop.
Refill fills vacant FIFO slots in order.
It returns allocation and WQ post errors explicitly.
A partial refill keeps the buffers that it already posted.
An unposted allocation remains safe to free after a rejected post.
Refill failure never revokes caller ownership of a delivered packet.
The caller must retry refill or stop the queue.
The API does not hide allocation errors behind successful packet delivery.

## Completion checks

The shared CQ layer checks owner bits, client type, WQ ID, and RQ direction.
RX accepts ordinary type 1 and truncated type 4.
A vendor error, unknown type, or unsupported coalesced type quarantines ownership.
RX checks its oldest posted offset and basic-unit count against the shared WQ tail before retirement.
It never retires a guessed or partial WQE.

The client header occupies bytes 0 through 3.
Receive flags occupy bytes 4 through 7.
The ordinary packet length occupies the low 16 bits at byte 8.
Its reserved length bits and the reserved word at byte 12 must be zero.
RX ignores checksum and hash metadata rather than advertising offloads.
It drops VLAN-stripped packets because this slice cannot restore their Ethernet header.
Unused packet-info entries do not determine ownership for an ordinary completion.

RX requires a complete Ethernet header and a length within both frame capacity and allocation bounds.
A known ordinary or truncated completion proves FIFO buffer completion.
Thus an invalid frame length causes a drop, not an unsafe delivery.
Identity, owner, type, and posted-record failures retain all device-owned buffers.

The pin defines `rx_wqe_offset` at byte 56 but does not define its units or validity rules.
Linux does not use this field in `mana_process_rx_cqe`.
RX follows the source-proven FIFO behavior and does not enforce a guessed interpretation.
Host tests use a nonzero arbitrary value to make this decision explicit.
This differs from TX completion offsets.
Do not apply TX offset rules to RX.

## Shutdown and lifetime

`mana_rx_stop` prevents refill and disables RX steering.
It quiesces the shared queue and sends the RX fence command.
The HWC acknowledgment alone never completes shutdown.
The bounded control wait services ordinary RX completions and the data CQ fence.
During shutdown, RX discards ordinary packets rather than transferring new ownership.
Only `mana_data_poll_cq` with `MANA_DATA_RX_FENCE` completes the fence.
The shared layer checks fence type 3, client type 1, zero vendor error, WQ identity, and RQ direction.

After the fence, stop destroys the shared RX object.
Only successful destroy and shared CQ notification cleanup permit release of remaining posted buffers.
A valid firmware failure keeps resources and permits a stop retry.
A timeout, malformed completion, clock error, or uncertain destroy quarantines resources.
No RX API frees quarantined device-owned buffers.
HWC teardown does not prove that data DMA has stopped.
An operator must prove device isolation before any external release.

Delivered packets can outlive RX, shared data, control, and bootstrap shutdown.
Their saved release callback and allocator context must remain live until packet release.
The allocator must also remain usable for that period.
The current Unikraft adapter stores its context and callback table in static storage.
`mana_unikraft_stop` clears bootstrap state, not the allocator.
Its packet release function uses only that retained allocator.
This permits release after stop within the allocator's lifetime.
It does not permit release after allocator or guest teardown.
Other platform implementations must supply the same lifetime or retain a shutdown guard.

## Evidence and hardware limits

`tests/test_rx.c` keeps its packet device model local.
It reuses the host bootstrap response hook and links production RX, queues, control, PCI, and GDMA.
The tests check golden one-SGE fields, synthetic DMA addresses, packet transfer, and release after full device stop.
They check FIFO and ring wrap through all owner values, rejected full WQs, and explicit refill retry.
They cover malformed lengths, identities, owners, headers, reserved fields, and posted records.
They cover truncated packets, unsupported types, allocation failures, missing fences, malformed fences, and uncertain destroy.
Firmware failures retain ownership for retry.

Run `make test`, `make test-sanitize`, and `make unikraft-test`.
The pinned Unikraft check compiles the adapter and links RX exports.
Host checks do not prove device DMA coherency, Azure queue acceptance, interrupt delivery, or RX traffic.
Direct VF setup still needs Azure evidence.
The driver still has no network interface, synthetic fallback, or VF removal recovery.
