# MANA uknetdev integration

## Scope and API pin

Ticket `1b4c15deff` connects the reviewed control, shared queues, TX, and RX APIs to uknetdev.
The integration adds no protocol commands, ring formats, or completion rules.
It uses Unikraft `e31b2c44effe583eb8910e713fda814b6e049213`.
The checked files are `lib/uknetdev/include/uk/netdev{,_core,_driver}.h`, `netbuf.h`, `netdev.c`, and `netbuf.c`.

The driver exposes one vPort and one TX/RX queue pair.
Each queue uses 4 KiB rings and 1 through 32 packet descriptors.
Zero descriptor counts select 32.
RX reserves CQ space for its fence.
TX supports either basic short or long OOB through the existing TX API.
Packets use one SGE and include the Ethernet header.

Capabilities report one queue pair, zero offload features, no packet headroom requirement, and byte alignment.
The maximum MTU comes from the queried frame MTU minus the 14-byte Ethernet header.
MAC and current MTU getters use control discovery.
MAC and promiscuous setters remain unsupported.
MTU changes require stopped queues and change the local packet limit only.
RSS, packet chains, checksum offloads, segmentation, VLAN insertion, VMBus fallback, and VF recovery remain unsupported.
The driver advertises no packet interrupts.
All calls need serialization outside interrupt context.

## Registration and lifecycle

Use `include/uk/mana/netdev.h`.
Supply fresh, zeroed `mana_netdev` storage, a ready bootstrap, and a live allocator.
`mana_netdev_register` starts control before it calls the actual `uk_netdev_drv_register`.
It returns the registered device ID or a negative errno.
Registration failure attempts control cleanup and retains resources if cleanup fails.
No registration occurs with a failed or DMA-blocked bootstrap.
The PCI adapter starts bootstrap and registers this interface.
It sets the PCI device running state only after both operations succeed.

Use the usual core operations in this order:

1. Call `uk_netdev_probe`.
2. Call `uk_netdev_configure` with one RX queue and one TX queue.
3. Configure TX queue zero.
4. Configure RX queue zero with `alloc_rxpkts` and its context.
5. Call `uk_netdev_start`.

Queue configuration saves descriptor counts and the RX allocator callback.
It allocates no device DMA.
Start creates the data EQ, starts TX, then starts RX and enables RX steering.
Failed start runs the same teardown path as stop.
Restart remains blocked while cleanup owns resources.

The pinned `uk_netdev_ops` has no stop callback.
Call `mana_netdev_stop` for driver stop.
Successful stop restores the core's `CONFIGURED` state and keeps queue configuration and control.
Then `uk_netdev_start` can create fresh queues.
Repeated stop succeeds without repeating object destruction.
A failed stop leaves callbacks inactive and permits only cleanup retry.
The core can still report `RUNNING` after failed stop because the driver has not completed teardown.
Packet callbacks return an explicit stopped or channel error in that state.

Stop disables RX, quiesces RX, waits for its validated data CQ fence, and destroys RX.
It reuses the existing bounded fence wait.
One service call drains at most one CQ ring.
Stop then drains TX with the required completion callback and destroys TX.
Only then does it release the shared data EQ and regions.
It retains uncertain DMA and accepted TX cookies.
It never bypasses control or bootstrap ownership guards.

`mana_netdev_shutdown` also releases control.
The caller can then stop bootstrap.
The PCI termination callback uses this full order.
Shutdown is terminal.
The pinned core has no unregister API.
Keep registered device storage, its allocator, and its callbacks alive.
The interface stays registered but inactive after shutdown.
The PCI adapter rejects a second add rather than overwrite that storage.
Cached MAC, MTU, and capability values remain valid after shutdown.

## Packet ownership

TX rejects packet chains, invalid buffer bounds, and unsupported offload flags before acceptance.
It services previous completions before submitting a new packet.
The TX API borrows packet bytes while it copies them into coherent DMA.
The netdev becomes the owner of one original netbuf reference only after submit accepts its cookie.
It does not add a reference or release the netbuf during submission.
This also preserves the pinned core's post-callback statistics access.
Queue-full status and negative errors leave ownership with the caller.
The `MORE` flag follows `mana_tx_free_packets` from [Transmit datapath](tx.md).
It needs a free record and enough WQ space for the next complete WQE.

Poll and stop return each proven TX cookie exactly once.
The completion handler logs packet errors and calls the real `uk_netbuf_free`.
Existing extra netbuf references remain valid.
Quarantine retains cookies whose device ownership remains uncertain.
Call `mana_netdev_poll` while the queues run to release completed TX packets and process control events.
Polls before the first start, while stopped, or during cleanup return `-ESHUTDOWN`.
The driver reports no platform error event for these returns.
Both packet callbacks also service TX.

RX poll transfers a coherent allocation to the integration through `mana_rx_packet`.
The integration calls the queue's saved `alloc_rxpkts` callback for one caller netbuf.
It checks that the returned linear buffer and its usable `len` hold the complete frame.
It copies packet bytes without changing `buf`, `buflen`, `data`, or private stack metadata.
It sets `len` to the actual frame length and clears offload flags.
It then releases the transferred DMA allocation exactly once and refills RX.
This copy preserves the caller's netbuf allocator and destructor contract.
It does not substitute a DMA-backed netbuf for stack-specific metadata.
Caller netbufs can outlive stop if their own allocator remains live.
No transferred DMA packet remains with the application.

Netbuf allocation failure drops the transferred packet and returns `UNDERRUN`.
An insufficient netbuf returns `-EMSGSIZE` without packet delivery.
DMA refill failure after delivery returns `SUCCESS | UNDERRUN` and preserves the delivered netbuf.
The platform logs refill errors.
Later calls retry refill or report the failed channel.
RX does not advertise speculative `MORE` without a second proven packet.

## Evidence and remaining gates

Run the host and sanitizer suites:

```sh
make test
make test-sanitize
```

Use an existing prepared core without changing it:

```sh
make test test-sanitize UK_SOURCE=/absolute/path/to/prepared/unikraft
make unikraft-test UK_SOURCE=/absolute/path/to/prepared/unikraft
```

The host suite compiles the production integration and all real MANA layers.
It also compiles the pinned `netdev.c` and `netbuf.c`, including real reference counting and public state checks.
Only host configuration, assertions, logging, and unused guest CPU header dependencies use test support headers.
The allocator uses the real `uk_alloc` layout and callback signatures.
No competing netdev or netbuf facade implements the tests.
The full Unikraft image compiles and links with real guest headers and no host substitutions.

Deterministic cases cover registration errors, the default DMA gate, configuration, default queue capacity, repeated lifecycle, and every startup allocation.
They also cover TX reference ownership, queue-full rejection, packet errors, RX lengths, allocator errors, refill failure, and release after stop.
They also compare the `MORE` flag with `mana_tx_free_packets` at both WQE sizes and check that idle polls report no platform error event.
Proven destroy failures permit retries.
Missing RX fences and bounded TX drain timeouts retain resources and block lower teardown.
The existing RX backlog test still checks a fence behind 10000 ordinary completions.
The host disposes quarantined model resources only after it models external device isolation.
Production provides no such release path.

No Azure device has validated this interface.
`LIBMANA_DIRECT_DMA` remains disabled by default.
Deployment must prove the DMA contract in [Bootstrap](bootstrap.md) before probe can succeed.
Link speed does not prove carrier.
Only source-defined control events establish known link state.
Azure gates still include HWC startup, real packet fields, bidirectional traffic, fenced RX shutdown, TX drain, and DMA isolation.
