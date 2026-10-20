# Unikraft MANA driver

This project targets the Azure MANA VF (`1414:00ba`) on x86_64 KVM.
The code implements PCI probe, GDMA/HWC bootstrap, MANA control, shared data queues, and basic TX/RX.
It registers one `uknetdev` after bootstrap and control succeed.
Production bootstrap rejects DMA unless `LIBMANA_DIRECT_DMA` enables an explicit deployment contract.
This option is disabled by default.
No Azure device has validated the contract or HWC startup.

The MVP scope is one vPort, one TX/RX queue pair, and basic Ethernet packets.
HWC requires at least one MSI-X vector.
RSS, offloads, multi-SGE packets, and synthetic VMBus/NetVSC fallback are out of scope.
The MVP does not promise network continuity during VF removal.

## Layout

| Path | Purpose |
|---|---|
| `Config.uk`, `Makefile.uk`, `exportsyms.uk` | Unikraft configuration, source list, and exported symbols |
| `Kraftfile` | Library metadata and exact Unikraft base |
| `include/uk/mana.h` | Public build scope and VF identifiers |
| `include/uk/mana/gdma.h` | Confirmed GDMA protocol units |
| `include/uk/mana/platform.h`, `bootstrap.h` | Platform contract, bootstrap state, and command API |
| `src/mana.c` | Build metadata |
| `src/pci.c`, `src/gdma.c` | PCI resources, MSI-X, SMC, HWC rings, and GDMA discovery |
| `include/uk/mana/control.h`, `src/control.c` | Device discovery, port setup, link events, and queue object control |
| `include/uk/mana/data_queue.h`, `src/data_queue.c` | Registered data rings, data EQ/CQ polling, and generic GDMA WQE submission |
| `include/uk/mana/tx.h`, `src/tx.c` | Copied TX frames and explicit completion ownership |
| `include/uk/mana/rx.h`, `src/rx.c` | RX buffer ownership, refill, and fenced shutdown |
| `include/uk/mana/netdev.h`, `src/netdev.c` | Real uknetdev operations, packet ownership, and driver stop API |
| `src/platform_unikraft.c` | Pinned x86_64 KVM platform and PCI driver registration |
| `tests/test_scaffold.c` | Host header and library smoke test |
| `tests/host/`, `tests/test_host.c` | Test runner, fault points, MMIO, allocation, and clock foundation |
| `docs/host-tests.md` | Host test contracts and device model limits |
| `tests/unikraft/` | Minimal application for the real Unikraft compile and link check |
| `tests/test_msix.c`, `tests/test_pic.c`, `tests/msix/` | Actual patch checks with IRQ, CPUID, and PIC I/O mocks |
| `patches/` | Required local xPIC MSI-X patch |
| `docs/firmware-tolerance.md` | Per-case inventory of strict firmware rejections versus the Linux pin, with decisions and Azure gates |
| `docs/platform.md` | Platform API findings and implementation constraints |
| `docs/bootstrap.md`, `tests/test_bootstrap.c` | Bootstrap contract, source pin, and focused protocol tests |
| `docs/control.md`, `tests/test_control.c` | Control contract, source pin, ownership, and focused tests |
| `docs/data-queues.md`, `tests/test_data_queue.c` | Shared queue ownership, packet worker boundary, and focused tests |
| `docs/netdev.md`, `tests/test_netdev.c` | Netdev lifecycle and real pinned uknetdev/netbuf host tests |

## Host checks

Use a C11 compiler, GNU make, and Python 3:

```sh
make test
make test-sanitize
make license-check
```

The tests do not need Azure credentials.
The netdev suite needs the prepared pinned Unikraft source.
The first run downloads that public source if the default cache does not exist.
Set `UK_SOURCE` to use an existing prepared source tree without changing it.
Both host targets run the foundation self-tests.
Both targets also compile the real integration, pinned `netdev.c`, and pinned `netbuf.c`.
See [Host test foundation](docs/host-tests.md) for fault schedules and mock limits.
The reusable bootstrap model covers GDMA/HWC transport.
Host tests do not prove DMA coherency or actual packet traffic.
`make clean` removes host test binaries. It keeps the downloaded core.
The license check covers project-owned C headers, sources, and build scripts.
New code uses BSD-3-Clause. Keep notices when adapting third-party code.

## Unikraft compile and link check

The exact base is `e31b2c44effe583eb8910e713fda814b6e049213` (version 0.21.0).
Do not replace it with `stable` or a version range.
The library needs the local patch. Kraft metadata alone does not apply it.

```sh
make unikraft-test
```

This command downloads the pinned public source, checks its SHA-256 checksum,
applies the patch, configures the test application, and builds an x86_64 KVM image.
It also checks the patched MSI-X source with mock IRQ and CPUID APIs.
These checks cover APIC address encoding, IRQ bounds, and allocation failures.
The actual patched PIC source also runs under UBSan.
It checks legacy IRQ 15 and MSI-X boundaries 16, 20, and 207.
It needs `curl`, `tar`, `sha256sum`, `patch`, and the Unikraft build tools.
Install GCC, binutils, flex, bison, m4, and Python 3 before this check.
The build selects GNU C11 because this core predates GCC's default C23 mode.
The test references the PCI registration and MSI-X symbols to check their ABI.
The smoke application does not emulate or attach a MANA device.
It links the real netdev integration without the host support headers.
The linked PCI driver rejects device DMA unless the deployment option is enabled.
The local patch and configuration support single-core guests only.

The generated tree and image stay under `build/`.
An existing unstamped core tree causes an error rather than an automatic overwrite.
To rebuild the image without fetching the core again, run `make unikraft-test`.

## Next milestones

The Fossil wiki pages **Azure MANA Driver Plan** and
**MANA and GDMA Protocol Research** define the work order and protocol sources.
Scaffold ticket: `b310ab6e80`.
PCI/GDMA ticket `64fdb04ff0` covers the implemented bootstrap paths.
See [Bootstrap](docs/bootstrap.md) for source evidence, tests, and remaining hardware gates.
Control ticket `6684b33f38` adds source-defined device and port operations.
See [Control](docs/control.md) for queue ownership and the TX/RX worker boundary.
See [Shared data queues](docs/data-queues.md) for the TX/RX prerequisite API and DMA release rules.
See [TX](docs/tx.md), [RX](docs/rx.md), and [Netdev](docs/netdev.md) for packet ownership and lifecycle rules.
Full device mocks, CI, and Azure validation remain separate tickets.
