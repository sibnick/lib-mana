# MANA userspace VFIO runner

Ticket `50b980feba` needs validation against a real MANA VF
(`1414:00ba`). The original plan ran the Unikraft image inside nested
QEMU on an Azure host. That plan is not possible: an Azure guest cannot
pass its Hyper-V SR-IOV VF through a second hypervisor. See
[Live run status](azure-validation.md#live-run-status-2026-10-21-paused).

The runner replaces that plan. It links the real driver sources
(`src/*.c`) against a `mana_platform` implementation that talks to the VF
through VFIO. It runs in the Linux host userspace, in one process, with
no QEMU and no Unikraft. It reports how deep the protocol stack works on
real firmware.

```text
 mana-vfio-runner (one Linux process)
   gates: probe -> gdma -> context -> packet
   src/*.c          real driver, unmodified
   tools/vfio/      platform backend: VFIO + THP DMA + eventfd IRQ
        | pread/pwrite/ioctl on the vfio device fd
        v
   vfio-pci bound MANA VF (1414:00ba)
```

## Who does what

The coordinator owns the device handover. The runner never binds or
unbinds drivers. Unbinding the VF cuts the VM network, so the coordinator
does the unbind, starts a watchdog, and restores `mana` afterwards.

The coordinator prepares the VF like this (as root):

```sh
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
BDF=$(lspci -Dnd 1414:00ba | awk 'NR==1{print $1}')
echo vfio-pci > /sys/bus/pci/devices/$BDF/driver_override
echo $BDF > /sys/bus/pci/drivers/mana/unbind
echo $BDF > /sys/bus/pci/drivers/vfio-pci/bind
```

On kernels older than 6.2 the same parameter lives under
`/sys/module/vfio_iommu_type1/parameters/`. On the target kernel
(`7.0.0-1017-azure`) it lives in the vfio core module.
`CONFIG_VFIO_NOIOMMU=y` is required. Enabling the mode prints a kernel
taint and a warning to the console. The runner prints the `echo` command
above when it sees `EPERM` on a noiommu group. It never runs the command
itself. If the parameter was off when the VF was bound, no group node
exists and the VF must be rebound.

## Security notes for noiommu mode

- Noiommu mode gives the process raw device access without IOMMU
  protection. The VF can DMA over all guest physical memory. Run the
  runner only on a disposable test VM, as root, with a watchdog.
- The group path requires `CAP_SYS_RAWIO`.
- MSI-X eventfds work in noiommu mode in current mainline kernels.
  Nothing in `drivers/vfio/pci/vfio_pci_intrs.c` blocks MSI-X for
  noiommu containers. A Hyper-V guest without interrupt remapping is
  still the risky case, so the runner treats it as one. See IRQ modes.
- After a failed run, rebind the VF to `mana`. The function reset at
  bind time returns the VF to a clean state.

## Build and run

```sh
make vfio-runner        # builds build/tools/mana-vfio-runner
sudo ./build/tools/mana-vfio-runner probe 0000:78:00.0
```

The Makefile links the binary statically when the local toolchain has a
static libc. The binary then runs on hosts without matching glibc.

Usage:

```text
mana-vfio-runner <gate> <bdf>
  gate: probe | gdma | context | packet | selftest
  bdf:  [domain:]bus:device.function  (lspci -Dnd 1414:00ba)
```

The BDF is always an argument. On Azure the bus number varies. The first
live test found the VF at `78:00.0` behind the Hyper-V virtual host
bridge.

Each gate run executes all earlier gates first, so one run reports the
deepest passing gate.

| Exit code | Meaning |
|---|---|
| 0 | Every requested gate passed or skipped |
| 1 | `probe` failed |
| 2 | `gdma` failed, or a usage error (usage errors print no `RUNNER_DONE`) |
| 3 | `context` failed |
| 4 | `packet` failed |

Environment:

| Variable | Default | Purpose |
|---|---|---|
| `MANA_VFIO_TIMEOUT_MS` | 5000 | Wait budget for one protocol step. Every expiry prints a `WAIT_TIMEOUT` line with the waited work and the variable name. Range 100 to 1200000. |

## Evidence lines

The runner writes its contract on stdout, one event per line:

```text
GATE_PROBE PASS detail=id=0x00ba1414 bars=bar0:0x... msix=64 ...
IRQ_MODE=msix-eventfd
GATE_GDMA PASS detail=eq=132 cq=129 ... irq_events=57
GATE_CONTEXT PASS detail=pd=1 mkey=3 ... mac=aa:bb:... vport=0x...
GATE_PACKET PASS detail=tx=complete rx_seen=0 ...
RUNNER_DONE packet
```

Verdicts are `PASS`, `FAIL err=-N`, or `SKIP reason`. A `SKIP` means the
device or firmware rejected a step for environmental reasons while the
driver stayed clean. `RUNNER_DONE <stage>` names the deepest gate that
passed or skipped. `TEARDOWN <op> ...` lines report best-effort release.
A quarantine note tells the coordinator to rebind the VF.

The `selftest` gate needs no VF. It proves the DMA allocator path
(2 MiB-aligned VMA, THP `madvise`, page touch, `mlock`, pagemap PFN
contiguity check) on the build machine. Without `CAP_SYS_ADMIN` the
kernel hides PFN bits, so the selftest reports
`pfn=hidden-unprivileged` and passes on the rest. The live run uses
root, where the full contiguity proof runs.

## IRQ modes

The probe stage answers one question: does
`VFIO_DEVICE_SET_IRQS` accept an MSI-X eventfd on this kernel and host?

- `IRQ_MODE=msix-eventfd`: the kernel programs the physical table entry
  and converts device interrupts into eventfd signals. The runner drains
  the eventfd inside `delay_us`, which the driver calls on every poll
  iteration. This is the real interrupt delivery path.
- `IRQ_MODE=poll-fallback`: the kernel refused `SET_IRQS`. The runner
  still registers a handler and calls it from its poll loop. The driver
  needs a change of its interrupt counter for the HWC and data EQ
  self-tests, and polling cannot produce that change by itself. The
  runner counts one delivery per wait. The driver still validates every
  event against the real EQ and CQ rings, so the gates still exercise
  real firmware. What is not validated: that the device could raise an
  interrupt. The runner prints this note loudly, and the `IRQ_MODE=`
  line tells the reviewer which mode produced the verdicts.
- `IRQ_MODE=none`: no MSI-X vector exists. Gates that need interrupt
  delivery fail with a clear verdict instead of passing silently.

## MMIO, config, and MSI-X table facts

The runner uses `pread` and `pwrite` on the vfio device fd for config
space and BAR access. It never uses `mmap`. Verified against the kernel
source (`drivers/vfio/pci/vfio_pci_rdwr.c`):

- A `pwrite` that overlaps the MSI-X table window is dropped. A `pread`
  there returns ones. This is intentional in `do_io_rw`. The driver's
  table writes in `setup_msix` therefore land nowhere, and its masked
  checks on vectors 1..n-1 pass on the all-ones reads. The real vector
  zero is already programmed by the kernel at `SET_IRQS` time, so the
  interrupt path works anyway. This matches how QEMU and DPDK treat the
  table.
- Qword MMIO through the region fd issues one 8-byte write only on
  kernels that carry the vfio qword IO path. Older kernels split the
  doorbell into two 4-byte writes. The target kernel (`7.0.0-azure`)
  carries the qword path.
- BAR sizing through emulated config space works. The kernel's
  `vfio_bar_fixup` answers the driver's `0xFFFFFFFF` sizing writes with
  the size mask, including for virtual functions.
- Config widths 1, 2, and 4 and MMIO widths 4 and 8 match the vfio
  access rules. The driver only issues naturally aligned accesses.

## DMA allocation and pagemap proof

`dma_alloc` must return coherent memory, zeroed, 4 KiB aligned, with a
device address below 2^52 (the GDMA cap). In noiommu mode the device
address must equal physical memory, so the runner proves contiguity
before it hands the address over:

1. `mmap` a VMA of at least 2 MiB, rounded and aligned to 2 MiB. A THP
   needs such a VMA. The target kernel has THP mode `[madvise]`, which
   fits this design.
2. `madvise(MADV_HUGEPAGE)`, then touch every page. The kernel places
   the 2 MiB huge page physically contiguous.
3. `mlock` the range. If the kernel refuses, the runner continues
   unlocked and counts it. Azure test VMs run without swap.
4. Read `/proc/self/pagemap` for every 4 KiB chunk. Each entry must be
   present, not swapped, and continue the page frame number sequence.
   The head PFN shifted by 12 is the device address.
5. On a placement failure (split pages, scattered PFNs) unmap and retry,
   up to 8 times, then fail with `ENOMEM`.

PFNs are visible only to a reader with `CAP_SYS_ADMIN` (root on the
live run). Without it the entries read as present with PFN zero. The
allocator then fails with `-EACCES` and tells the operator to run as
root. Allocations up to 2 MiB land inside one THP in practice. Larger
requests need adjacent THPs and usually fail with `ENOMEM` on a
fragmented host. The runner uses 4 KiB allocations everywhere, like the
host end-to-end test.

Inside an Azure guest there is no vIOMMU between the VF and
guest-physical memory, so the process PFN is the frame the VF sees.

## Contract findings

The runner must not change `src/*`. These findings record what the
platform contract needs from a bare VFIO host:

1. The driver requires a real interrupt count change. `mana_gdma_start`
   (HWC EQ self-test) and the data EQ self-test in `mana_data_start`
   wait for `b->interrupts != before`. Only a platform handler call
   changes that counter. A platform cannot pass those gates without
   handler deliveries, so the poll-fallback mode supplies them and says
   so. A platform that never delivers must fail those gates visibly.
2. MSI-X table access through the platform mapping is a no-op under
   VFIO (writes dropped, reads return ones). The driver still needs the
   mapping to exist and the masked checks to pass. It does: `setup_msix`
   reads all-ones and sees every extra vector masked.
3. `map(physical, size)` receives the BAR address from config space.
   A VFIO platform must resolve that address to a region itself. The
   runner caches each BAR base and size at device open and matches on
   both.
4. The driver's wait loops are abortable: every loop passes through
   `delay_us` and propagates its error. The runner enforces
   `MANA_VFIO_TIMEOUT_MS` inside `delay_us` by returning `-ETIMEDOUT`
   after the step deadline. No source change was needed.
5. `mana_pci_start` returns `-EBUSY` when bus master is already set. A
   `VFIO_DEVICE_RESET` at open clears it, so a VF released by the
   runner is reusable without a rebind.
6. The runner registers only MSI-X vector 0. That matches the driver,
   which routes the bootstrap EQ and the shared data EQ to vector 0.

## What this runner does not cover

- The unikraft glue stays out of scope: the xPIC MSI-X patch, the
  ukalloc-backed DMA provider, and `uknetdev` registration. The netdev
  layer stays covered by the host netdev suite.
- The runner validates the driver core against firmware. It does not
  validate `CONFIG_LIBMANA_DIRECT_DMA` deployment logic.
- RX success depends on live subnet traffic. The packet gate reports
  `rx_seen=0` without failing when the subnet is quiet. TX completion
  must arrive.
