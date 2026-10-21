# Azure MANA validation runbook

This runbook covers ticket `50b980feba`. A unikraft guest runs the
`samples/networkload` image inside nested QEMU on an Azure Ubuntu VM
(the host). The real MANA VF serves the guest through `vfio-pci`. A
second small VM (the peer) on the same subnet pings the guest and
opens a TCP connection to port 8080. The guest must answer both.

```text
 peer VM (B2s) ── ping + TCP 8080 ──► subnet ──► MANA VF (1414:00ba)
                                                  │ vfio-pci
                                                  ▼
                                     host VM (D2ds_v5, nested KVM)
                                                  │ qemu -device vfio-pci
                                                  ▼
                                     unikraft networkload guest
```

The scripts wrap the `az` CLI. They read all settings from the
environment. The scripts print created names and a cleanup summary.
The provisioning and teardown scripts also need `AZ_CONFIRM=yes`,
because they create or delete billable resources.

## Prerequisites

1. Azure CLI, a subscription with quota for the sizes below, and
   rights to create resource groups.
2. Nested virtualization: the host VM size must expose virtualization
   extensions to the guest. Microsoft documents nested virtualization
   support for the Dv5, Dsv5, Ddv5, Ddsv5, Dasv5, Dadsv5, Ev5, Edsv5,
   and the v6 general purpose series. B-series sizes do not support
   it. `Standard_D2ds_v5` qualifies. The host setup script checks
   `/dev/kvm` and stops early if the size does not deliver.
3. MANA hardware: the host VM must be placed on hardware that offers
   the MANA VF (`lspci -d 1414:00ba`). Microsoft began placement on
   MANA-capable hardware for Intel v5 series sizes on 2026-05-26, and
   the Intel v6 series always uses MANA. Placement is not guaranteed
   for one deployment. If the host shows no MANA VF, resize or
   re-provision the host on a v6 size, then repeat the host setup.
4. Ubuntu 24.04 LTS (GA kernel 6.8): the kernel includes the in-tree
   `mana` driver, so an Azure MANA placement keeps host networking
   up. The validation moves the VF away from `mana` only for the
   test window.
5. vIOMMU: `vfio-pci` normally needs an IOMMU. An Azure VM usually
   does not expose a virtual IOMMU to the guest, so expect the host
   setup script to report no IOMMU lines. In that case run the test
   with `AZ_ALLOW_NOIOMMU=yes`. That setting enables
   `vfio` unsafe noiommu mode inside the guest-facing window only.
   It is acceptable on this throwaway test bed, and it is wrong for
   any shared system.
6. Local build tools for the image: `curl`, `tar`, `sha256sum`,
   `patch`, `unzip`, `flex`, `bison`, `m4`, `python3`, GCC, binutils,
   and network access. `make workload-image` downloads the pinned
   Unikraft core, the pinned lib-lwip port, and the lwIP sources.

## Step 1: Build the guest image

From the repository root:

```sh
make workload-image
sha256sum build/workload/networkload_qemu-x86_64
```

The image is a 32-bit multiboot ELF, usually well under 1 MiB. The
guest boots with the static address from the kernel command line.
The supported key is `netdev.ip=<ip>/<maskbits>:<gateway>`. See
`samples/networkload/README.md` for the verified mechanism on the
pinned core.

## Step 2: Provision the test bed

```sh
AZ_SUB=<subscription> AZ_LOC=westeurope AZ_PREFIX=manaval \
AZ_CONFIRM=yes scripts/azure_provision.sh
```

The script creates the resource group, a VNet with one subnet, an
NSG that allows only intra-subnet ICMP and TCP, a NAT gateway for
outbound traffic, a storage account for boot diagnostics and serial
console logs, the host VM (`Standard_D2ds_v5`, static IP
10.77.1.10), and the peer VM (`Standard_B2s`, static IP 10.77.1.11).
Neither VM gets a public IP or an inbound SSH port. The script prints
the private IPs. Keep them for the next steps.

## Step 3: Prepare the host VM

```sh
AZ_RG=manaval-rg AZ_HOST_VM=manaval-host \
scripts/azure_host_setup.sh
```

The payload installs QEMU and tools, checks nested virtualization,
checks for an IOMMU in `dmesg`, finds the MANA PCI id, loads the
vfio modules, and writes three helper scripts under
`/usr/local/sbin`: `mana-vfio-bind`, `mana-vfio-unbind`, and
`mana-guest-stop`. A successful run ends with a `SETUP_OK` line.
Exit code 10 means the size gives no nested virtualization. Exit
code 11 means the VM sits on NetVSC hardware: see prerequisite 3.

## Step 4: Run the validation

Upload the image where the host can reach it, for example a private
blob, then run:

```sh
az storage blob upload --resource-group manaval-rg \
  --account-name manavaldiag -c kernels \
  -n networkload --file build/workload/networkload_qemu-x86_64
AZ_URL=$(az storage blob url --account-name manavaldiag \
  -c kernels -n networkload --query url -o tsv)
az storage blob generate-sas ... # private SAS for the upload URL

AZ_RG=manaval-rg AZ_HOST_VM=manaval-host AZ_PEER_VM=manaval-peer \
AZ_HOST_IP=10.77.1.10 \
AZ_KERNEL_URL="$AZ_URL" \
AZ_KERNEL_SHA256=<sha256 from step 1> \
scripts/azure_run_test.sh
```

For a small image you can skip storage and pass the file directly:

```sh
AZ_KERNEL_B64_FILE=build/workload/networkload_qemu-x86_64
```

Add `AZ_ALLOW_NOIOMMU=yes` when step 3 warned about a missing IOMMU.

The script runs four phases:

1. It uploads the image to the host, writes the detached test
   driver, and starts it with `systemd-run`. The driver unbinds the
   VF from `mana`, binds `vfio-pci`, starts QEMU with the image, and
   waits for the guest marker line.
2. It waits for the guest to boot.
3. It probes from the peer: `ping` and one TCP connect to port 8080
   with the expected `mana-ok` reply.
4. It polls the host for the final report, then stops the guest and
   rebinds `mana`.

While the VF serves the guest, the host has no network. The Azure
WireServer cannot reach the host agent, so run-command calls fail or
hang in that window. The script expects this and polls. The report
arrives once the driver rebinds `mana`. A watchdog on the host
restores the driver within `AZ_WATCHDOG_MIN` minutes even if this
script dies. As a last resort, `az vm restart` restores the host,
because the vfio bind is runtime state only.

### Expected results

The peer prints:

```text
PING_PASS
TCP_PASS
```

The host report prints:

```text
BOOT_PASS
TEST_WINDOW_OPEN ...
TEST_WINDOW_CLOSE ...
RESTORE_PASS
TEST_DONE ...
```

The script ends with a summary line and one final line:
`MANA_VALIDATION=PASS` or `MANA_VALIDATION=FAIL`. The guest serial
log stays on the host at `/root/mana-guest-serial.log`. It shows the
`en0: Set IPv4 address ...` line and one answered-check line per TCP
probe. A boot diagnostics serial console can show the host console
if a run-command payload fails.

### Step 4b: Userspace VFIO run (no nested VM)

The nested path needs a guest vIOMMU, which Azure does not expose to
Linux VMs today. The userspace path runs the same driver sources as a
Linux binary through `vfio-pci` in `noiommu` mode. It validates the
protocol against real firmware. See `docs/vfio-runner.md` for the gate
contract and the security note.

Build the runner locally and publish it to the diagnostic storage
account. `az vm run-command` silently drops payloads over about
1.6 MB, so the binary travels as a gzipped blob and the host fetches
it over a SAS URL:

```sh
make vfio-runner
strip build/tools/mana-vfio-runner
gzip -9n -c build/tools/mana-vfio-runner > /tmp/mana-vfio-runner.gz
az storage blob upload -c vfio -n mana-vfio-runner.gz \
  --file /tmp/mana-vfio-runner.gz \
  --account-name <account> --overwrite
```

Run it against the live VF. `RUNNER_SHA256` covers the stripped,
uncompressed binary; the host verifies it after gunzip:

```sh
AZ_RG=manaval3-rg AZ_HOST_VM=manaval3-peer \
AZ_GW=10.77.1.1 \
RUNNER_URL='<blob SAS URL, single-quoted>' \
RUNNER_SHA256=<sha256 of the stripped binary> \
VFIO_STAGES="probe gdma context packet" \
scripts/azure_run_vfio.sh
```

The script arms a detached driver, moves the VF from `mana` to
`vfio-pci`, runs the gates, restores `mana`, and prints the report.
Exit 11 means this host placement has no MANA VF. Delete the VM and
create a fresh one to re-roll placement; redeploy does not move the
NIC pool. Then run Step 3 again.

## Step 5: Tear down

```sh
AZ_RG=manaval-rg AZ_CONFIRM=yes scripts/azure_teardown.sh
```

The script lists the resources, deletes the resource group, and
verifies that it is gone.

## Security posture

- No public IP on either VM. No inbound SSH. The peer reaches the
  host only through the private subnet.
- The NSG allows only intra-subnet ICMP and intra-subnet TCP
  inbound. Everything else is denied inbound.
- One Standard public IP exists on the NAT gateway, for outbound
  traffic only (packages and the guest image download).
- The storage account blocks public blob access. Use a private SAS.
- The validation window runs with the VF detached from the host.
  Nothing else on the host keeps network during that window.

## Live run status: 2026-10-21, full protocol stack passes

The test bed lives in westus2 (`manaval3-rg`). The peer VM
`manaval3-peer` landed on MANA-capable hardware, so it now hosts the
test. The former host `manaval3-host` (NetVSC placement) is
deallocated. The VF reports at `7870:00:00.0` (VPCI virtual domain).

Results:

| Check | Result |
|---|---|
| MANA placement on D2ds_v5 | PASS on the peer placement. |
| Guest vIOMMU | FAIL. Empty /sys/class/iommu. Nested VFIO stays blocked. |
| Userspace VFIO bind (noiommu) | PASS. Group `0`, node `/dev/vfio/noiommu-0`. |
| GATE_PROBE on real firmware | PASS. id 0x00ba1414, 1024 MSI-X, BAR0 32 MB, doorbell 0x1000000/0x1000, shm 0x820000. |
| MSI-X eventfd delivery | IRQ_MODE=msix-eventfd. Works without interrupt remapping. |
| GATE_GDMA | PASS. SMC wait, HWC init, QUERY_DEVICE. dev_id=2, depth=4, limits sq/rq/cq=1024 eq=128 db=4096. |
| GATE_CONTEXT | PASS. PD, mkey, doorbell page, vport. Real MAC, MTU 1500, link 12500 Mbps. |
| GATE_PACKET | PASS. TX accepted: CQE type 32 (TX_OKAY), error 0. The firmware adds vendor_err 0xe00001, an informational note that the port reports like the pin. RX queues create, refill, and steer clean. |

Final clean run: all four gates PASS, every teardown step ok,
`RUNNER_DONE packet`, `RESTORE_PASS attempt=1`.

The test bed was torn down on 2026-10-22 and ticket `50b980feba`
closed Fixed. To repeat a run: `scripts/azure_provision.sh`, then
`scripts/azure_host_setup.sh`, then Step 4b. MANA placement on a size
is a hardware lottery. Redeploy does not move a VM to another NIC
pool; delete and create a fresh VM to re-roll placement.

Solved on the way: the numeric sysfs group versus `noiommu-N` node
naming, the kernel-owned MSI-X control bits (the backend virtualizes
them and mirrors Enable into SET_IRQS), the double release of vector 0
at teardown (the kernel answers the second release with EINVAL), and
the post-reset control read of zero (seed the table size from the VFIO
vector count). ICMP is blocked on this guest for both the subnet
gateway and the WireServer, so the restore check probes the IMDS
endpoint over TCP instead.

## Subscription constraints

New subscriptions can block resources that the scripts need. Observed on
a fresh subscription in October 2026:

| Constraint | Symptom | Resolution |
|---|---|---|
| Region closed to new customers | VNet create fails with `RequestDisallowedByAzure` | Provision in an eligible region, for example westus2. Quota is per region. |
| Microsoft.Storage not registered | Storage account create fails with `SubscriptionNotFound` | `az provider register --namespace Microsoft.Storage`, wait for `Registered`, rerun. The script is rerunnable. |
| B-series not available | Peer VM create fails with `NotAvailableForSubscription` | Set `AZ_PEER_SIZE` to an unrestricted size. |
| SKU capacity restricted | vmCreate fails with `SkuNotAvailable`, and az CLI can hide it behind a Python traceback | Read the raw error: `az rest --method put` on the VM URL prints the JSON. Set `AZ_PEER_SIZE=Standard_D2ds_v5` (same size as the host) when B-series and A-series fail. |
| Regional core quota is small | VM create fails on quota | The default test bed needs 4 cores: host 2, peer 2. Check `az vm list-usage`. |

## Known risks

| Risk | Effect and handling |
|---|---|
| Host placed on NetVSC hardware | No MANA VF. Host setup exits 11. Delete and create a fresh VM; redeploy does not move the NIC pool. |
| No vIOMMU exposed to the host VM | `vfio-pci` bind fails. Run with `AZ_ALLOW_NOIOMMU=yes` on this disposable test bed. |
| Coordinator dies mid-run | The detached driver restores `mana` on exit, and a host watchdog restores it after `AZ_WATCHDOG_MIN` minutes. `az vm restart` is the last resort. |
| Stale SDN state after rebind | The host may need seconds to renew its address. The report pings the gateway and the WireServer and prints `RESTORE_PASS` or `RESTORE_FAIL`. |
| ICMP blocked on the guest | The subnet gateway and the WireServer both drop echo requests on a healthy network. The restore check probes the IMDS endpoint (169.254.169.254) over TCP. |
| run-command silently drops large payloads | The upload phase fails or the file on the VM is short. The payload ceiling is about 1.6 MB. Ship the runner as a gzipped blob behind `RUNNER_URL`. |
| Failed transient unit poisons the unit name | A retry prints `START_ARMED` but starts nothing. `systemctl status mana-vfio-run` shows `failed`. The coordinator clears the failed state before each start. |
| run-command fails during the VF window | Expected. The poll loop retries until the report appears or `AZ_TEST_TIMEOUT` expires. |
| Peer probe misses the window | Increase `AZ_TEST_WINDOW`. The driver keeps the guest up for the whole window. |
| lwIP port downloads its upstream sources from a moving branch | The lib-lwip port checks out the `UNIKRAFT-2_1_x` fork branch without a checksum. Record the built image hash on the ticket. |
| QEMU default CPU model lacks x2APIC | The driver needs x2APIC for MSI-X. The run script passes `-cpu host`. |
| Linker warning `requires executable stack` | Known pinned-core build note. It does not affect the test. |
