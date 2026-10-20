#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Prepare the MANA validation host VM through az vm run-command.
# The payload installs QEMU, checks nested virtualization, checks the
# vIOMMU, finds the MANA VF, loads the vfio modules, and writes the
# vfio bind, unbind, and guest stop helper scripts on the host.
# This script creates no billable Azure resources.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: AZ_RG=<resource group> AZ_HOST_VM=<host VM> \
       scripts/azure_host_setup.sh

Required environment:
  AZ_RG         Resource group of the host VM.
  AZ_HOST_VM    Host VM name.

Optional environment:

Exit codes from the host payload:
  10  nested virtualization is not usable (no vmx or svm, or no /dev/kvm)
  11  no MANA VF (1414:00ba) visible: the VM sits on NetVSC hardware
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
	usage
	exit 0
fi

fail() {
	echo "error: $*" >&2
	exit 1
}

[[ -n "${AZ_RG:-}" && -n "${AZ_HOST_VM:-}" ]] || {
	usage >&2
	fail "AZ_RG and AZ_HOST_VM are required"
}

payload=$(mktemp /tmp/mana-host-setup.XXXXXX.sh)
trap 'rm -f "$payload"' EXIT

cat > "$payload" <<'PAYLOAD_EOF'
#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive

echo "== Install QEMU and tools =="
apt-get update -y
apt-get install -y --no-install-recommends \
	qemu-system-x86 qemu-utils pciutils kmod curl \
	iputils-ping iproute2 systemd

echo "== Check nested virtualization =="
if ! grep -qE 'vmx|svm' /proc/cpuinfo; then
	echo "FAIL nested-virt: the CPU exposes neither vmx nor svm flags"
	exit 10
fi
modprobe kvm_intel 2>/dev/null || modprobe kvm_amd 2>/dev/null || true
if [ ! -c /dev/kvm ]; then
	echo "FAIL nested-virt: /dev/kvm is missing"
	exit 10
fi
echo "PASS nested-virt: /dev/kvm is present"

echo "== Check vIOMMU =="
dmesg | grep -i iommu | head -n 20 || true
if dmesg | grep -qiE 'DMAR: IOMMU enabled|AMD-Vi|Interrupt remapping'; then
	echo "PASS viommu: the kernel reports an IOMMU"
else
	echo "WARN viommu: dmesg shows no IOMMU lines"
	echo "WARN vfio-pci binding can fail in this state."
	echo "WARN Options: add intel_iommu=on iommu=pt to grub and reboot,"
	echo "WARN or run azure_run_test.sh with AZ_ALLOW_NOIOMMU=yes."
fi

echo "== Find the MANA VF =="
BDF=$(lspci -d 1414:00ba | awk '{print $1}' | head -n 1)
if [ -z "$BDF" ]; then
	echo "FAIL mana-vf: lspci shows no 1414:00ba device"
	echo "FAIL mana-vf: the host VM runs on NetVSC hardware"
	echo "FAIL mana-vf: resize or re-provision on a MANA-capable size"
	exit 11
fi
echo "PASS mana-vf: 0000:$BDF"

echo "== Load vfio modules =="
modprobe vfio
modprobe vfio_iommu_type1
modprobe vfio-pci
printf 'vfio\nvfio_iommu_type1\nvfio-pci\n' > /etc/modules-load.d/mana-vfio.conf

cat > /usr/local/sbin/mana-vfio-bind <<'EOS'
#!/bin/sh
# Unbind the MANA VF from the mana driver and bind it to vfio-pci.
set -eu
BDF="${1:-$(lspci -d 1414:00ba | awk '{print $1}' | head -n 1)}"
[ -n "$BDF" ] || { echo "error: no MANA VF found" >&2; exit 1; }
BDF="0000:${BDF#0000:}"
DEV="/sys/bus/pci/devices/$BDF"
IFACE=$(ls "$DEV/net" 2>/dev/null | head -n 1 || true)
if [ -n "$IFACE" ]; then
	ip link set "$IFACE" down 2>/dev/null || true
fi
if [ -e "$DEV/driver" ]; then
	CUR=$(basename "$(readlink -f "$DEV/driver")")
	if [ "$CUR" = "vfio-pci" ]; then
		echo "PASS vfio-bind: $BDF already bound to vfio-pci"
		exit 0
	fi
	echo "$BDF" > "$DEV/driver/unbind"
fi
echo vfio-pci > "$DEV/driver_override"
if ! echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null; then
	echo "1414 00ba" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
	sleep 1
	echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind
fi
echo "PASS vfio-bind: $BDF bound to vfio-pci"
EOS

cat > /usr/local/sbin/mana-vfio-unbind <<'EOS'
#!/bin/sh
# Unbind the MANA VF from vfio-pci and bind it back to the mana driver.
set -eu
BDF="${1:-$(lspci -d 1414:00ba | awk '{print $1}' | head -n 1)}"
[ -n "$BDF" ] || { echo "error: no MANA VF found" >&2; exit 1; }
BDF="0000:${BDF#0000:}"
DEV="/sys/bus/pci/devices/$BDF"
if [ -e "$DEV/driver" ]; then
	CUR=$(basename "$(readlink -f "$DEV/driver")")
	if [ "$CUR" = "vfio-pci" ]; then
		echo "$BDF" > "$DEV/driver/unbind"
	fi
fi
echo > "$DEV/driver_override" 2>/dev/null || true
if ! echo "$BDF" > /sys/bus/pci/drivers/mana/bind 2>/dev/null; then
	modprobe mana 2>/dev/null || true
	sleep 1
	echo "$BDF" > /sys/bus/pci/drivers/mana/bind
fi
echo "PASS vfio-unbind: $BDF rebound to mana"
EOS

cat > /usr/local/sbin/mana-guest-stop <<'EOS'
#!/bin/sh
# Stop the nested unikraft guest and its watchdog. Never unbinds a VF.
systemctl stop mana-guest.service 2>/dev/null || true
systemctl stop mana-watchdog.timer 2>/dev/null || true
systemctl stop mana-watchdog.service 2>/dev/null || true
echo "PASS guest-stop: nested guest units are stopped"
EOS

chmod +x /usr/local/sbin/mana-vfio-bind \
	/usr/local/sbin/mana-vfio-unbind \
	/usr/local/sbin/mana-guest-stop

echo "SETUP_OK qemu=$(qemu-system-x86_64 --version | head -n 1) mana=0000:$BDF"
echo "Created on host: /usr/local/sbin/mana-vfio-bind"
echo "Created on host: /usr/local/sbin/mana-vfio-unbind"
echo "Created on host: /usr/local/sbin/mana-guest-stop"
echo "Created on host: /etc/modules-load.d/mana-vfio.conf"
PAYLOAD_EOF

echo "== Running host setup on $AZ_HOST_VM (group $AZ_RG) =="
az vm run-command invoke \
	--resource-group "$AZ_RG" --name "$AZ_HOST_VM" \
	--command-id RunShellScript --scripts "@$payload"

cat <<EOF

Created resources:
  host files under /usr/local/sbin and /etc/modules-load.d, listed above.
Cleanup summary:
  These files live on the host VM. They disappear with the VM.
  Run scripts/azure_teardown.sh to delete the resource group.
EOF
