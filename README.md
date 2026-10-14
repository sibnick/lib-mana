# Unikraft MANA driver scaffold

This project targets the Azure MANA VF (`1414:00ba`) on x86_64 KVM.
The current code is a **library scaffold, not a working network driver**.
It does not probe PCI devices, start GDMA/HWC, or register a network interface.

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
| `src/mana.c` | Build metadata, without device initialization |
| `tests/test_scaffold.c` | Host header and library smoke test |
| `tests/unikraft/` | Minimal application for the real Unikraft compile and link check |
| `tests/test_msix.c`, `tests/msix/` | Host checks of the actual patch with IRQ and CPUID mocks |
| `patches/` | Required local xPIC MSI-X patch |
| `docs/platform.md` | Platform API findings and implementation constraints |

## Host checks

Use a C11 compiler, GNU make, and Python 3:

```sh
make test
make test-sanitize
make license-check
```

The tests do not need Azure credentials or a network connection.
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
It needs `curl`, `tar`, `sha256sum`, `patch`, and the Unikraft build tools.
Install GCC, binutils, flex, bison, m4, and Python 3 before this check.
The build selects GNU C11 because this core predates GCC's default C23 mode.
The test references the PCI registration and MSI-X symbols to check their ABI.
It never programs a PCI device.
The local patch and configuration support single-core guests only.

The generated tree and image stay under `build/`.
An existing unstamped core tree causes an error rather than an automatic overwrite.
To rebuild the image without fetching the core again, run `make unikraft-test`.

## Next milestones

The Fossil wiki pages **Azure MANA Driver Plan** and
**MANA and GDMA Protocol Research** define the work order and protocol sources.
Scaffold ticket: `b310ab6e80`.
PCI/GDMA ticket `64fdb04ff0` must implement platform DMA ownership, resource
validation, MSI-X table programming, bounded HWC startup, and failure cleanup.
Control, TX/RX, `uknetdev`, full device mocks, and Azure validation remain separate tickets.
