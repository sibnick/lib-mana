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

## Known risks

| Risk | Effect and handling |
|---|---|
| Host placed on NetVSC hardware | No MANA VF. Host setup exits 11. Re-size or re-deploy on a v6 size. |
| No vIOMMU exposed to the host VM | `vfio-pci` bind fails. Run with `AZ_ALLOW_NOIOMMU=yes` on this disposable test bed. |
| Coordinator dies mid-run | The detached driver restores `mana` on exit, and a host watchdog restores it after `AZ_WATCHDOG_MIN` minutes. `az vm restart` is the last resort. |
| Stale SDN state after rebind | The host may need seconds to renew its address. The report pings the gateway and prints `RESTORE_PASS` or `RESTORE_FAIL`. |
| run-command fails during the VF window | Expected. The poll loop retries until the report appears or `AZ_TEST_TIMEOUT` expires. |
| Peer probe misses the window | Increase `AZ_TEST_WINDOW`. The driver keeps the guest up for the whole window. |
| lwIP port downloads its upstream sources from a moving branch | The lib-lwip port checks out the `UNIKRAFT-2_1_x` fork branch without a checksum. Record the built image hash on the ticket. |
| QEMU default CPU model lacks x2APIC | The driver needs x2APIC for MSI-X. The run script passes `-cpu host`. |
| Linker warning `requires executable stack` | Known pinned-core build note. It does not affect the test. |
