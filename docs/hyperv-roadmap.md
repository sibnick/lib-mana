# Hyper-V Platform Roadmap

Native Azure deployment path: the unikernel runs as an Azure VM. The
partition itself owns the MANA VF. No nesting, no VFIO passthrough, no
vIOMMU.

Status date: 2026-10-22.

## 1. Premise check (facts, not opinion)

1. Unikraft upstream has **no Hyper-V platform**. The `plat/` tree holds
   only `common`, `kvm`, `native`, and `xen`. Tracking issue
   `unikraft/unikraft#61` opened in 2020 and stayed open. No pull
   request exists for a hyperv platform.
2. The only prior code is `paulvlase/plat-hyperv-old`. It has four
   commits. It targets a local Windows Hyper-V host, not Azure. It never
   completed the VMBus root channel handshake. The last update was
   2022-05. The Unikraft VMM working group minutes from 2023-01 confirm
   channel initialization did not work.
3. Microsoft contributed Hyper-V work to the **Linux kernel**, not to
   Unikraft: `drivers/pci/controller/pci-hyperv.c` (vPCI frontend with
   the DMA enrollment state machine) and the `mana` driver. Use both as
   the protocol oracle for this project.
4. The newest core release is `RELEASE-0.21.0` "Ijiraq" (2026-05-20).
   That is the version this library already pins. No core port is needed
   today. Release 0.21.0 also introduced the Platform Abstraction Layer
   (`lib/ukpal`) and the generic arch API. That is the interface a new
   platform now plugs into. This lowers the cost of `plat/hyperv`
   compared with every earlier attempt.
5. MANA VFs appear on x86_64 Azure sizes. Our live validation ran on
   `Standard_D2ds_v5` (see `docs/azure-validation.md`). ARM64 is not
   required.

Consequence: Option 3 is not a port. It is an implementation of the
Hyper-V guest stack for Unikraft. The MANA driver part is finished and
hardware-proven. Everything under it must be built.

## 2. Scope control

lib-mana needs only four platform services. The roadmap builds nothing
else:

| Service        | KVM today (VFIO runner proves semantics) | hyperv target              |
|----------------|------------------------------------------|----------------------------|
| Device find    | PCI host bridge scan                     | vPCI enumeration by class  |
| Config space   | ECAM / `pread` on vfio fd                | vPCI config message RPC    |
| MMIO (GDMA BAR)| `mmap` of VFIO region                    | mapped BAR + MMIO register |
| DMA + MSI-X    | `VFIO_IOMMU_MAP_DMA`, eventfds           | plain GPA, SynIC messages  |

Out of scope: storage, netvsp, keyboard/mouse, VTL isolation, confidential
VMs, ARM64, SMP boot on hyperv.

## 3. Platform facts that shape the plan

1. Console: the Azure portal serial console is the VMBus text console
   VSP. It shows nothing from a legacy COM port. A unikernel that wants
   portal-visible logs must implement the VMBus console early. Early
   boot output can use the Gen1 VGA text buffer.
2. Boot chain: Azure Gen1 VMs boot legacy BIOS from a fixed-size VHD.
   Unikraft x86_64 images are multiboot ELF kernels. The VHD needs a
   GRUB install with multiboot support. This is a spike, not assumed.
3. No simulator: QEMU does not model VMBus. Every bring-up iteration is
   a VHD upload plus an Azure boot. The plan makes the push pipeline
   scriptable and fast before the protocol phases start.
4. DMA: on a non-isolated VM, the hypervisor translates device DMA
   through the partition SLAT. Plain guest physical addresses work.
   Linux registers GPA ranges with `MAP_GPA_RANGE` hypercalls only for
   hardening and isolated VMs. Phase 5 must confirm this on Azure before
   we skip registration.
5. Interrupts: vPCI delivers device MSI as VMBus messages on the
   vPCI channel. The guest maps them to SynIC SINT pages. lib-mana uses
   MSI-X per queue. The message path must support multiple vectors.

## 4. Phases

Each phase ends with a hard gate. Stop or re-plan at a failed gate.
Estimates assume one experienced engineer, full time.

### Phase 0 - Design and upstream contact (0.5 week)

1. Comment on `unikraft/unikraft#61`. Ask whether upstream wants
   `plat/hyperv` and under what constraints. Decide fork-first,
   upstream-later.
2. Write the design note: platform layout under `plat/hyperv` on the
   0.21.0 PAL. Define the minimal boot flow and memory map.

Gate: design note committed. Upstream reply recorded, silence accepted
after two weeks.

### Phase 1 - Boot spike on Azure (1-2 weeks)

1. Build the stock `helloworld` Unikraft app for `kvm` x86_64, then for
   a stub hyperv platform (no Hyper-V logic yet, console on VGA text).
2. Produce the Gen1 fixed VHD with GRUB multiboot. Script:
   `qemu-img convert -O vpc -o subformat=fixed`, upload,
   `az image create`, `az vm create` on `Standard_D2ds_v5`.
3. Run. Read the portal boot diagnostics or screen capture. Issue the
   Hyper-V `HVCALL_POST_MESSAGE`-free shutdown or power off with az.

Gate: the stub unikernel boots as a custom image on MANA hardware.
The script tears everything down after each run.

### Phase 2 - Hyper-V core bring-up (2-3 weeks)

1. CPUID feature detection. Hyper-V identification and hypercall page
   (XMM fast call mode).
2. SynIC init on VPs: message page, event page per vCPU, SINT1 routed to
   the vMBus message vector. `HVCALL_POST_MESSAGE` and
   `HVCALL_SIGNAL_EVENT` working.

Gate: INITCONTACT / TELLVERSION on the root channel returns a version
answer. This is the exact point where the old fork died. Treat it as the
highest-risk step in the project. Budget slack here.

### Phase 3 - VMBus core plus console (3-4 weeks)

1. Ring buffer open/close, GPADL registration, packet framing, send
   completion, channel suspend/resume handling.
2. Offer/respind handling, `CHANNELMSG_ENUMOFFER` walk.
3. VMBus text console VSP client. Register it as the ukconsole device.

Gate: the portal serial console shows unikernel logs. Offer list prints
the vPCI provider GUID (`DAA2...-...-pci`) plus netvsp.

### Phase 4 - vPCI frontend (3-4 weeks)

1. Protocol handshake: query protocol, query HRDK, query rel IDs.
2. Register the guest MMIO window (REGISTER_MMIO). Read BARs.
3. Synchronous config space RPC per access (or batched reads).
4. Enumerate function 0 devices. Find vendor `0x1414`, device `0x00ba`.
   Map the GDMA BAR into the unikernel address space.

Gate: the driver reads the GDMA doorbell BAR and the device registers.
The VFIO gate `GATE_PROBE` (read `manabar` + caps) passes inside Azure.

### Phase 5 - DMA and interrupts on vPCI (2-4 weeks)

1. Confirm plain GPA DMA. If Azure rejects device DMA, implement the
   Linux `MAP_GPA_RANGE` enrollment sequence for guest RAM.
2. vPCI interrupt types: CREATE_INTERR, request MSI per vector, map
   SynIC message slots to lib-mana IRQ hooks.
3. Port the runner gate suite: run GATE_PROBE, GATE_GDMA, GATE_CONTEXT
   inside the VM. Compare with the run-10 values in
   `docs/azure-validation.md` (`dev_id=2`, `depth=4`, limits
   sq/rq/cq=1024, eq=128, db=4096, TX `CQE_TX_OKAY` + vendor note).

Gate: GDMA adapter init plus TX/RX context bring-up passes. Same
teardown sequence as the VFIO runner passes.

### Phase 6 - lib-mana netdev end to end (1-2 weeks)

1. Implement the platform ops for hyperv. Replace the VFIO glue in
   `src/platform_unikraft.c` via the PAL. Keep one common core.
2. Run the lwip sample: get a DHCP or static lease over MANA. Ping the
   peer VM. Transfer volume.

Gate: peer-to-peer traffic passes on two Azure VMs. `RUNNER_DONE packet`
equivalent in VM logs.

### Phase 7 - Harden and upstream (2+ weeks, can interleave)

1. Clean room re-runs, repeated boot matrix, error-path teardown.
2. Propose `plat/hyperv` upstream on `unikraft#61`. Offer the code and
   the Azure test procedure.
3. Keep the VFIO runner as the offline regression harness for the
   driver core.

## 5. Total estimate and kill criteria

Realistic total: 12 to 16 engineering weeks of focused work, with a
real probability of overrun in Phases 2 and 4. This is a platform
project, not a glue port. Kill or re-scope criteria:

1. Phase 2 gate fails after four weeks of tries: the VMBus handshake
   does not complete. Stop. Re-assess with Microsoft contacts.
2. Azure image pipeline blocks the iteration loop (quota, approval,
   region): the feedback cycle exceeds one hour per run. Automate first,
   then continue.
3. Upstream states a firm requirement we cannot meet in `plat/`: keep
   the platform in this repository tree permanently.

## 6. What is already ready

1. The MANA driver core: platform-independent, proven on live hardware
   with the VFIO runner (run 10, 2026-10-21). TX semantics aligned with
   firmware behavior (`CQE_TX_OKAY` equals success).
2. The Azure tooling: provisioning, host setup, test driver, teardown.
   All scripts exist in `scripts/` and the validation runbook is
   `docs/azure-validation.md`.
3. The pinned core: 0.21.0 is the newest release and already carries the
   PAL. No version chase is needed to start.

## 7. Decisions the owner must make

1. Budget: accept a 3 to 4 month platform project, or keep the VFIO
   runner as the interim story.
2. Upstream posture: contribute `plat/hyperv` to Unikraft, or keep it a
   private platform.
3. Architecture line: x86_64 Gen1 only for v1. ARM64 Gen2 later.
4. Ticket structure: open one epic ticket plus one ticket per phase when
   work starts.
