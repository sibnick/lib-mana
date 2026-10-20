# Network load sample

This sample proves bidirectional networking over the MANA `uknetdev`.
It answers ICMP echo requests and answers TCP probes on port 8080.
Each TCP probe gets one `mana-ok` line, then the guest closes the
connection. The sample targets one core on the x86_64 KVM platform.
It uses the pinned Unikraft base and the pinned lib-lwip port.

## Network configuration mechanism

The pinned core passes the kernel command line to the `uklibparam`
parser at early boot. With `CONFIG_LIBUKNETDEV_EINFO_LIBPARAM=y`, the
`uknetdev` library registers one kernel parameter array under the
`netdev` prefix. The accepted keys are:

| Key | Meaning |
|---|---|
| `netdev.ip=<ip>/<maskbits>` | Static IPv4 address in CIDR form |
| `netdev.ip=<ip>/<maskbits>:<gateway>` | Also sets the IPv4 gateway |
| `netdev.ip=<ip>:<gw>:<dns0>:<dns1>:<host>:<domain>` | Legacy dotted form and extra fields |

One `netdev.ip=` entry configures one netdev, in netdev registration
order. The MANA device is the only netdev in this defconfig, so the
first entry configures it. The lib-lwip autoiface step reads these
values during boot, sets the address, and brings the interface up.
`mana4=`, `managw4=`, and `mananm4=` are not supported. The pinned
core has no such keys. DHCP is disabled on purpose. Azure SDN only
routes the private IP that Azure assigns to the VM.

Example boot line:

```text
netdev.ip=10.77.1.10/24:10.77.1.1
```

## Build

From the repository root, run:

```sh
make workload-image
```

This target reuses the pinned Unikraft tree of `make unikraft-test`.
It also downloads the pinned lib-lwip tree, applies its fetch and
patch rules, and builds the image under `build/workload/`. Set
`UK_SOURCE` to reuse an existing prepared core tree. Set
`UK_LWIP_SOURCE` to reuse an existing lib-lwip tree at commit
`ec55ae17618feeb57c8c10109bcf5c42723e8e95` (RELEASE-0.21.0). The
build needs network access. The lwIP port downloads the upstream
lwIP sources during the build.

The image is `build/workload/networkload_qemu-x86_64`.

## Run

Start the image on a Linux host with KVM and with the MANA VF bound
to `vfio-pci`:

```sh
qemu-system-x86_64 -enable-kvm -m 1024 -smp 1 -display none \
	-serial file:guest.log -kernel build/workload/networkload_qemu-x86_64 \
	-append "netdev.ip=10.77.1.10/24:10.77.1.1" \
	-device vfio-pci,host=0000:01:00.0
```

A peer on the same subnet then runs ping and one TCP connect to port
8080. See `docs/azure-validation.md` for the full Azure runbook.

## Expected serial output

The guest prints these lines, in order:

```text
MANA netload 1414:00ba, Unikraft e31b2c44...
Registered netdev0: ... (mana)
MANA: uknetdev registered, MANA device ...
en0: Set IPv4 address 10.77.1.10 mask 255.255.255.0 gw 10.77.1.1
en0: Interface is up
netload: TCP check server is ready on port 8080
```

Each peer probe adds one `netload: answered one TCP check` line.
