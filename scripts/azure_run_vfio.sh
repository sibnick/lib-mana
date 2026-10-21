#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Run the userspace VFIO runner (make vfio-runner) against the live MANA
# VF on the host VM. This is the non-nested validation path: the driver
# sources run as a Linux userspace binary through vfio-pci in noiommu
# mode. See docs/vfio-runner.md for the contract and the security note.
#
# The host loses network while the VF is away from the mana driver. The
# work runs detached through systemd-run. A watchdog restores the driver
# even if this coordinator dies. Polls between phases tolerate failures.
set -euo pipefail

AZ_RG="${AZ_RG:-}"
AZ_HOST_VM="${AZ_HOST_VM:-}"
RUNNER_B64_FILE="${RUNNER_B64_FILE:-}"
RUNNER_SHA256="${RUNNER_SHA256:-}"
VFIO_STAGES="${VFIO_STAGES:-probe gdma context packet}"
WATCHDOG_MIN="${AZ_WATCHDOG_MIN:-15}"
POLL_TRIES="${AZ_POLL_TRIES:-40}"
GW="${AZ_GW:-}"

fail() { echo "ERROR: $*" >&2; exit 1; }
[[ -n "$AZ_RG" && -n "$AZ_HOST_VM" && -n "$RUNNER_B64_FILE" ]] || \
	fail "AZ_RG, AZ_HOST_VM, RUNNER_B64_FILE are required"
[[ -f "$RUNNER_B64_FILE" ]] || fail "runner file not found: $RUNNER_B64_FILE"

work=$(mktemp -d /tmp/mana-run-vfio.XXXXXX)
trap 'rm -rf "$work"' EXIT
note_on_exit() {
	local rc=$?
	if [[ "$rc" -ne 0 ]]; then
		cat >&2 <<EOF

Recovery note: the host driver rebinds the mana driver on exit and a
watchdog rebinds it within $WATCHDOG_MIN minutes. If the host network
stays down, restart the VM from the Azure control plane:
  az vm restart --resource-group $AZ_RG --name $AZ_HOST_VM
EOF
	fi
}
trap 'note_on_exit' EXIT

run_host() {
	local raw
	raw=$(az vm run-command invoke --resource-group "$AZ_RG" \
		--name "$AZ_HOST_VM" --command-id RunShellScript \
		--scripts "@$1" --query "value[0].message" -o tsv 2>/dev/null) || return 1
	printf '%s' "$raw" | tr -d '\r' | \
		python3 -c 'import sys; sys.stdout.write(sys.stdin.buffer.read().decode("unicode_escape", "ignore"))' 2>/dev/null || \
		printf '%s\n' "$raw"
}

echo "== Phase 1: upload the runner binary and arm the test driver =="
payload_a="$work/arm.sh"
printf '#!/bin/sh\nset -eu\n' > "$payload_a"
{
	printf 'STAGES=%s\n' "'$VFIO_STAGES'"
	printf 'WATCHDOG_MIN=%s\n' "'$WATCHDOG_MIN'"
	printf 'GW=%s\n' "'$GW'"
	printf 'RUNNER_SHA256=%s\n' "'${RUNNER_SHA256}'"
} >> "$payload_a"
cat >> "$payload_a" <<'BODY_HEAD'
RUNNER=/usr/local/bin/mana-vfio-runner
base64 -d > "$RUNNER" <<'EOB64'
BODY_HEAD
base64 "$RUNNER_B64_FILE" >> "$payload_a"
cat >> "$payload_a" <<'BODY_TAIL'
EOB64
chmod 755 "$RUNNER"
if [ -n "$RUNNER_SHA256" ]; then
	printf '%s  %s\n' "$RUNNER_SHA256" "$RUNNER" | sha256sum -c -
fi
echo "RUNNER_OK size=$(wc -c < "$RUNNER")"

# Hyper-V virtualizes the PCI host bridge. The domain is not 0000. Keep
# the BDF exactly as lspci prints it (example: 7870:00:00.0).
bdf=$(lspci -Dnd 1414:00ba | head -n 1 | awk '{print $1}')
if [ -z "$bdf" ]; then
	echo "ARM_FAIL mana-vf: no 1414:00ba device on this host placement"
	exit 11
fi
echo "VF_BDF=$bdf"

cat > /root/mana-vfio-test.sh <<EOS
#!/bin/sh
set -u
exec > /root/mana-vfio.log 2>&1
echo "TEST_START \$(date -u +%FT%TZ)"
bdf='$bdf'
cleanup() {
	systemctl stop mana-vfio-watchdog 2>/dev/null || true
	pkill -f 'mana-vfio-runner' 2>/dev/null || true
	echo "\$bdf" > "/sys/bus/pci/drivers/vfio-pci/unbind" 2>/dev/null || true
	echo "" > "/sys/bus/pci/devices/\$bdf/driver_override" 2>/dev/null || true
	echo "\$bdf" > /sys/bus/pci/drivers/mana/bind 2>/dev/null || true
	sleep 10
	if [ -n '$GW' ] && ping -c 3 -W 2 '$GW' > /dev/null 2>&1; then
		echo "RESTORE_PASS"
	elif [ -z '$GW' ]; then
		echo "RESTORE_DONE"
	else
		echo "RESTORE_FAIL"
	fi
	echo "TEST_DONE \$(date -u +%FT%TZ)"
}
trap cleanup EXIT

# The Azure guest kernel keeps noiommu support in the vfio core module.
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
modprobe vfio-pci
echo "\$bdf" > "/sys/bus/pci/drivers/mana/unbind" || exit 1
echo vfio-pci > "/sys/bus/pci/devices/\$bdf/driver_override"
echo "\$bdf" > /sys/bus/pci/drivers/vfio-pci/bind || exit 1
echo "VFIO_BOUND \$(basename "\$(readlink -f "/sys/bus/pci/devices/\$bdf/driver")")"

systemd-run --unit=mana-vfio-watchdog --on-active=${WATCHDOG_MIN}min \\
	/bin/sh -c 'pkill -f mana-vfio-runner; echo $bdf > /sys/bus/pci/drivers/vfio-pci/unbind; echo "" > /sys/bus/pci/devices/$bdf/driver_override; echo $bdf > /sys/bus/pci/drivers/mana/bind'

for stage in $STAGES; do
	echo "---- stage \$stage ----"
	timeout 90 "$RUNNER" "\$stage" "\$bdf"
	rc=\$?
	echo "---- stage \$stage exit=\$rc ----"
	if [ "\$rc" -ge 124 ]; then
		echo "GATE_\$stage TIMEOUT"
	fi
done
echo "TEST_END \$(date -u +%FT%TZ)"
EOS
chmod 755 /root/mana-vfio-test.sh
echo "DRIVER_ARMED"
BODY_TAIL

out=$(run_host "$payload_a") || out=""
printf '%s\n' "${out:-}" | grep -q DRIVER_ARMED || {
	printf '%s\n' "${out:-}" | tail -20
	fail "arm phase failed (exit 11 means: no MANA placement on this host)"
}
printf '%s\n' "${out:-}" | grep -E "RUNNER_OK|VF_BDF" || true

echo "== Phase 2: start the detached run (the host network drops; expected) =="
payload_b="$work/start.sh"
cat > "$payload_b" <<'BODY_B'
rm -f /root/mana-vfio.log
systemd-run --unit=mana-vfio-run /root/mana-vfio-test.sh
echo START_ARMED
BODY_B
out=$(run_host "$payload_b") || out=""
printf '%s\n' "${out:-}" | grep -q START_ARMED || fail "start phase failed"

echo "== Phase 3: wait for TEST_DONE and the driver restore =="
done_marker=""
for i in $(seq 1 "$POLL_TRIES"); do
	sleep 20
	payload_c="$work/fetch.sh"
	cat > "$payload_c" <<'BODY_C'
cat /root/mana-vfio.log 2>/dev/null
echo FETCH_DONE_MARKER
BODY_C
	out=$(run_host "$payload_c") || out=""
	if printf '%s\n' "${out:-}" | grep -q "TEST_DONE"; then
		done_marker="yes"
		break
	fi
	printf '%s\n' "$out" | grep -q "FETCH_DONE_MARKER" && \
		echo "   poll $i: run still in flight" || \
		echo "   poll $i: host unreachable (VF window)"
done

echo "===== VFIO RUN REPORT ====="
if [[ -n "$done_marker" ]]; then
	printf '%s\n' "$out" | sed -n '/TEST_START/,/TEST_DONE/p'
else
	echo "report incomplete after $POLL_TRIES polls"
	printf '%s\n' "$out" | tail -20
fi
echo "============================"

if [[ -n "$done_marker" ]]; then
	printf '%s\n' "$out" | grep -E "GATE_[A-Z]+ (PASS|FAIL|SKIPPED|TIMEOUT)" || \
		echo "NO_GATE_LINES"
	printf '%s\n' "$out" | grep -E "RESTORE_(PASS|FAIL|DONE)" || true
	printf '%s\n' "$out" | grep -q "RUNNER_DONE" || echo "NOTE: no RUNNER_DONE line"
fi
