# MANA device and port control

## Scope and source

Ticket `6684b33f38` adds source-level control for one VF port and one TX/RX queue pair.
It does not add packet descriptors, packet delivery, or a network interface.
The platform still rejects DMA unless the default-off deployment gate has explicit evidence.
No Azure device has tested this control path.

Wire formats follow Linux check-in `0fcb05b72344f40806cbe810de8872f1698bf053`.
The implementation uses the BSD-3-Clause license option and keeps the Microsoft notice.
The source files are:

- `include/net/mana/mana.h`
- `include/net/mana/gdma.h`
- `include/net/mana/hw_channel.h`
- `drivers/net/ethernet/microsoft/mana/mana_en.c`
- `drivers/net/ethernet/microsoft/mana/gdma_main.c`
- `drivers/net/ethernet/microsoft/mana/hw_channel.c`

The driver encodes little-endian bytes, not native C structures.
Request and response versions can differ.
HWC accepts any response version of at least 1, like the pinned source.
It checks command, activity, device, completion queue, and status.
The pinned response readers never read the header type word or the reserved word.
A response below the expected struct size fails that command only.
It does not poison control. See [Firmware tolerance](firmware-tolerance.md).
Firmware status `0xffffffff` returns `-EOPNOTSUPP`.
Other firmware failures return `-EPROTO`.

| Command | Code | Request bytes/version | Response bytes/requested version |
|---|---|---|---|
| Register device | 4 | 40/1 | 44/1 |
| Deregister device | 5 | 40/1 | 32/1 |
| Query device | 0x20001 | 88/1 | 80/3 |
| Query port | 0x20008 | 44/1 | 64/1 |
| Configure TX | 0x20003 | 56/1 | 36/1 |
| Configure RX | 0x20007 | (120 + 8 * entry count)/5 | 40/2 |
| Query link | 0x2000a | 48/1 | 48/2 |
| Create WQ object | 0x20004 | 96/3 | 56/2 |
| Destroy WQ object | 0x20005 | 56/1 | 32/1 |
| Fence RX | 0x20006 | 48/1 | 32/1 |

## Device and port boundary

Use `include/uk/mana/control.h`.
Supply fresh, zeroed control and queue storage.
Serialize control calls with all bootstrap calls.
Do not call them from interrupt context.
Retain the bootstrap, platform callbacks, and storage until all ownership ends.

`mana_control_start()` registers the discovered MANA device.
It queries protocol 0.1.1 without unsupported driver capability flags.
It records four capability words, the port count, EQ limit, and adapter frame MTU.
It records the bare-metal host mode byte and continues, like the pin.
It rejects invalid resource fields and invalid port handles.
It keeps a zero or multicast MAC as reported and reports it once.
It queries the selected port and its queue limits.
It configures TX with the registered device's PD and doorbell, not the HWC bootstrap PD.
It stores the TX short-form byte as a boolean, like the pin.
It disables RX and queries link speed and QoS.
Every link query failure stays recorded in `link_query_error` and does not block start.
Only a transport poison fails start on that query.
See [Firmware tolerance](firmware-tolerance.md) for the per-case evidence.

MAC discovery does not provide a MAC setter.
The pin has no ordinary VF MAC-setting command.
The adapter MTU includes the 14-byte Ethernet header.
The local MTU excludes that header.
For old firmware or a zero MTU, the driver logs the source-defined 1514-byte frame limit.
An invalid nonzero MTU fails discovery.
`mana_control_set_mtu()` validates the local packet limit only when no queues exist.
The pin has no MTU-setting command.
The RX worker must size buffers for this limit.

Link speed does not prove carrier state.
HWC event 133, subtype 2 or 3, updates `bootstrap.link_known`, `link_up`, and `link_generation`.
These events apply to all ports in the Linux source.
Carrier remains unknown until a link event arrives.
`mana_control_poll()` processes events outside interrupt context.
Timeout-update events do not extend the existing command bound.
Reset, FPGA reconfiguration, and service events fail with `-ENOTSUP`.
Unknown events and malformed completions poison HWC.
The platform has no safe VF reset or removal recovery API.

## Queue ownership

The caller creates DMA regions and a parent EQ outside this API.
The [shared data queue layer](data-queues.md) supplies these resources and their complete ownership lifecycle.
Regions must belong to the registered PD.
The queue specification supplies region handles, byte sizes, a parent EQ ID, and a moderation context.
The driver requests no chaining, moderation, RSS, or completion coalescing.
Queue sizes must be power-of-two multiples of the 4 KiB page size.
The caller must service the parent EQ and data CQ.
Control does not dispatch data completions through the HWC CQ.
The parent EQ must differ from the HWC EQ.
Returned WQ/CQ IDs must not collide with HWC IDs in the same queue namespace.

`mana_control_create_queue()` returns the object handle and WQ/CQ IDs.
It supports one TX object and one RX object.
Success consumes both DMA region handles, as in Linux.
Do not send separate region-destroy commands for consumed handles.
Clear the caller's region handles after success.
Keep the backing DMA memory until object destroy succeeds.
The specification stored in the queue records the original handles for diagnostics only.

An uncertain create result quarantines the queue and both region handles.
Do not reuse, destroy, or free those regions or their memory.
Invalid identifiers in a successful response also quarantine control.
A firmware failure with a valid response leaves the regions with the caller.
Destroy failures retain the queue object and memory ownership.
A valid firmware failure permits a later retry.
A poisoned channel or malformed response does not permit a retry.

`mana_control_set_rx()` enables the live RX object as the default RX destination.
RSS remains disabled.
The enable request updates every indirection entry to the single RX object.
It uses the full source-defined table shape even with RSS disabled.
Disable requests retain the table and clear only steering state.
Invalid table counts use the source-defined 64-entry default and produce a log.
A table that exceeds the HWC request limit fails start with `-E2BIG`.
The current 4 KiB HWC request limit does not support a 512-entry table.
Disable RX before fencing or destroying its queue.
Stop TX submissions before destroying TX.
TX completion draining belongs to the TX worker.

`mana_control_fence_rx()` sends the fence command.
Its command response does not prove that DMA has stopped.
The RX worker must validate a `CQE_RX_OBJECT_FENCE` completion for the object.
It then calls `mana_control_fence_complete()`.
`mana_control_wait_fence()` calls a nonblocking CQ poll with a 10-second bound.
A missing completion, polling error, or clock error quarantines control and its queues.
RX destroy requires both disabled steering and a completed fence.

`mana_control_stop()` requires all queue objects to have ended.
It also requires the shared data queue context to release its EQ and all backing regions.
It disables RX and deregisters the device.
A failed start can still own device registration.
Call stop after a failed start unless control is quarantined.
Bootstrap stop returns `-EBUSY` while control owns the device.
Do not free caller-owned storage after any uncertain operation.
Quarantine has no source-proven reset path in this platform.
An operator must prove device DMA has stopped before releasing quarantined resources.
HWC teardown alone does not prove that data queues have stopped.

## Evidence and limits

`tests/test_control.c` uses the production control, PCI, and HWC source.
The reusable bootstrap model transports exact requests and injects transport faults.
The control response hook supplies only source-defined MANA responses.
The tests check wire lengths, versions, fields, reserved bytes, discovery, and queue state.
They also check firmware errors, malformed responses, timeouts, link events, and quarantine.

Run `make test`, `make test-sanitize`, and `make unikraft-test`.
The Unikraft check compiles and links control exports against the pinned core.
These checks cannot prove direct-DMA coherency or Azure port setup.
They cannot prove that Azure permits standalone VF operation without a synthetic path.
Ticket `64fdb04ff0` still needs hardware evidence.
Packet traffic, synthetic fallback, and VF removal remain outside this source slice.
