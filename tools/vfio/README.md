<!-- SPDX-License-Identifier: BSD-3-Clause -->

# VFIO gate runner

This directory holds a Linux userspace runner that validates the real
driver sources against a live MANA VF. It links `src/*.c` unmodified
against a VFIO platform backend.

| File | Purpose |
|---|---|
| `main.c` | Gate sequence (probe, gdma, context, packet), CLI, evidence lines, teardown |
| `platform.c` | `mana_platform` over VFIO: config, MMIO by pread/pwrite, THP plus pagemap DMA, MSI-X eventfd with poll fallback, step timeouts |
| `vfio-runner.h` | Shared context, gate exit codes, backend API |

Build and check:

```sh
make vfio-runner
make vfio-selftest
sudo ./build/tools/mana-vfio-runner probe <bdf>
```

Read [docs/vfio-runner.md](../../docs/vfio-runner.md) before a live run.
It covers the coordinator steps, the noiommu security notes, the IRQ
modes, and the evidence contract. The runner never binds or unbinds
drivers. It refuses a device that is still bound to `mana`.
