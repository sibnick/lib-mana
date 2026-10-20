#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Run the MANA pass-through validation on the host VM and the peer VM.
# Flow: the script uploads the unikraft kernel to the host, starts a
# detached test driver on the host, runs ping and TCP probes from the
# peer, then collects the host report. The detached driver rebinds the
# mana driver even if this script dies. The host loses network access
# while the VF serves the nested guest. run-command calls fail in that
# window. This script expects those failures and polls.
# This script creates no billable Azure resources.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: AZ_RG=<group> AZ_HOST_VM=<host> AZ_PEER_VM=<peer> \
       AZ_HOST_IP=<host private IP> (AZ_KERNEL_URL=<https url> | \
       AZ_KERNEL_B64_FILE=<local image>) scripts/azure_run_test.sh

Required environment:
  AZ_RG                Resource group of the VMs.
  AZ_HOST_VM           Host VM name. Runs the nested unikraft guest.
  AZ_PEER_VM           Peer VM name. Runs ping and the TCP probe.
  AZ_HOST_IP           Private IP that the VF serves in the guest.
  AZ_KERNEL_URL        https URL (for example a blob SAS URL) of the
                       unikraft image, or:
  AZ_KERNEL_B64_FILE   local build/workload/networkload_qemu-x86_64 file,
                       embedded as base64. Meant for small test files.

Optional environment, with defaults:
  AZ_SUBNET_CIDR       Subnet prefix, used for the mask. Default: 10.77.1.0/24
  AZ_GW                Gateway. Default: first address of a /24 subnet.
  AZ_KERNEL_SHA256     Expected SHA-256 of the kernel image.
  AZ_GUEST_MEM_MB      QEMU guest memory.     Default: 1024
  AZ_BOOT_TIMEOUT      Boot wait seconds.     Default: 90
  AZ_TEST_WINDOW       Probe window seconds. Default: 150
  AZ_BOOT_WAIT         Wait after arming.     Default: 40
  AZ_PEER_RETRIES      Peer probe rounds.    Default: 6
  AZ_POLL_INTERVAL     Host poll interval s. Default: 20
  AZ_TEST_TIMEOUT      Total wait seconds.   Default: 1500
  AZ_WATCHDOG_MIN      Restore watchdog min. Default: 15
  AZ_ALLOW_NOIOMMU     yes permits vfio noiommu fallback. Default: no
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

[[ -n "${AZ_RG:-}" && -n "${AZ_HOST_VM:-}" && -n "${AZ_PEER_VM:-}" ]] || {
	usage >&2
	fail "AZ_RG, AZ_HOST_VM, and AZ_PEER_VM are required"
}
[[ -n "${AZ_KERNEL_URL:-}${AZ_KERNEL_B64_FILE:-}" ]] || {
	usage >&2
	fail "set AZ_KERNEL_URL or AZ_KERNEL_B64_FILE"
}
[[ -z "${AZ_KERNEL_URL:-}" || "${AZ_KERNEL_URL}" =~ ^https:// ]] || \
	fail "AZ_KERNEL_URL must be an https URL"
if [[ -n "${AZ_KERNEL_B64_FILE:-}" ]]; then
	[[ -f "$AZ_KERNEL_B64_FILE" ]] || fail "AZ_KERNEL_B64_FILE not found: $AZ_KERNEL_B64_FILE"
	size=$(wc -c < "$AZ_KERNEL_B64_FILE")
	[[ "$size" -le 4194304 ]] || fail "kernel file exceeds 4 MiB; upload it with AZ_KERNEL_URL instead"
fi

HOST_IP="${AZ_HOST_IP:?}"
[[ "$HOST_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "AZ_HOST_IP is not an IPv4 address"
SUBNET_CIDR="${AZ_SUBNET_CIDR:-10.77.1.0/24}"
PREFIXLEN="${SUBNET_CIDR#*/}"
[[ "$PREFIXLEN" =~ ^[0-9]+$ ]] || fail "AZ_SUBNET_CIDR has no numeric prefix"
if [[ -n "${AZ_GW:-}" ]]; then
	GW="$AZ_GW"
elif [[ "$PREFIXLEN" == "24" ]]; then
	GW=$(printf '%s' "$SUBNET_CIDR" | awk -F'[./]' '{print $1"."$2"."$3".1"}')
else
	fail "set AZ_GW for a non-/24 subnet"
fi
[[ "$GW" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "gateway is not an IPv4 address"
[[ -z "${AZ_KERNEL_SHA256:-}" || "${AZ_KERNEL_SHA256}" =~ ^[0-9a-fA-F]{64}$ ]] || \
	fail "AZ_KERNEL_SHA256 must be 64 hex digits"

MEM_MB="${AZ_GUEST_MEM_MB:-1024}"
BOOT_TIMEOUT="${AZ_BOOT_TIMEOUT:-90}"
TEST_WINDOW="${AZ_TEST_WINDOW:-150}"
BOOT_WAIT="${AZ_BOOT_WAIT:-40}"
PEER_RETRIES="${AZ_PEER_RETRIES:-6}"
POLL_INTERVAL="${AZ_POLL_INTERVAL:-20}"
TEST_TIMEOUT="${AZ_TEST_TIMEOUT:-1500}"
WATCHDOG_MIN="${AZ_WATCHDOG_MIN:-15}"
ALLOW_NOIOMMU="${AZ_ALLOW_NOIOMMU:-no}"
CMD_TIMEOUT="${AZ_CMD_TIMEOUT:-900}"
[[ "$ALLOW_NOIOMMU" == "yes" || "$ALLOW_NOIOMMU" == "no" ]] || fail "AZ_ALLOW_NOIOMMU must be yes or no"

work=$(mktemp -d /tmp/mana-run-test.XXXXXX)
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

payload_a="$work/arm.sh"
payload_b="$work/peer.sh"
payload_c="$work/fetch.sh"

printf '#!/bin/sh\n' > "$payload_a"
{
	printf 'HOST_IP=%s\n' "'$HOST_IP'"
	printf 'PREFIXLEN=%s\n' "'$PREFIXLEN'"
	printf 'GW=%s\n' "'$GW'"
	printf 'MEM_MB=%s\n' "'$MEM_MB'"
	printf 'BOOT_TIMEOUT=%s\n' "'$BOOT_TIMEOUT'"
	printf 'TEST_WINDOW=%s\n' "'$TEST_WINDOW'"
	printf 'WATCHDOG_MIN=%s\n' "'$WATCHDOG_MIN'"
	printf 'ALLOW_NOIOMMU=%s\n' "'$ALLOW_NOIOMMU'"
	printf 'KERNEL_URL=%s\n' "'${AZ_KERNEL_URL:-}'"
	printf 'KERNEL_SHA256=%s\n' "'${AZ_KERNEL_SHA256:-}'"
} >> "$payload_a"

cat >> "$payload_a" <<'BODY_A_HEAD'
set -eu
KERNEL=/root/mana-kernel
echo "== Prepare guest kernel =="
if [ -n "$KERNEL_URL" ]; then
	curl -fsSL "$KERNEL_URL" -o "$KERNEL"
fi
BODY_A_HEAD

if [[ -n "${AZ_KERNEL_B64_FILE:-}" ]]; then
	printf 'base64 -d > "$KERNEL" <<"EOB64"\n' >> "$payload_a"
	base64 "$AZ_KERNEL_B64_FILE" >> "$payload_a"
	printf 'EOB64\n' >> "$payload_a"
fi

cat >> "$payload_a" <<'BODY_A_TAIL'
test -s "$KERNEL"
if [ -n "$KERNEL_SHA256" ]; then
	printf '%s  %s\n' "$KERNEL_SHA256" "$KERNEL" | sha256sum -c -
fi
echo "KERNEL_OK size=$(wc -c < "$KERNEL")"
bdf=$(lspci -d 1414:00ba | awk '{print $1}' | head -n 1)
if [ -z "$bdf" ]; then
	echo "ARM_FAIL mana-vf: no 1414:00ba device on the host"
	exit 11
fi
bdf="0000:${bdf#0000:}"
echo "== Write test driver (VF $bdf, guest IP $HOST_IP/$PREFIXLEN, gw $GW) =="
cat > /root/mana-test.sh <<EOS
#!/bin/sh
set -eu
exec > /root/mana-test.log 2>&1
echo "TEST_START \$(date -u +%FT%TZ)"
cleanup() {
	systemctl stop mana-watchdog.timer 2>/dev/null || true
	systemctl stop mana-watchdog.service 2>/dev/null || true
	systemctl stop mana-guest.service 2>/dev/null || true
	/usr/local/sbin/mana-vfio-unbind || true
	sleep 10
	if ping -c 3 -W 2 '$GW' > /dev/null 2>&1; then
		echo "RESTORE_PASS"
	else
		echo "RESTORE_FAIL"
	fi
	ip -4 -br addr show || true
	echo "TEST_DONE \$(date -u +%FT%TZ)"
}
trap cleanup EXIT
sleep 15
if ! /usr/local/sbin/mana-vfio-bind '$bdf'; then
	if [ '$ALLOW_NOIOMMU' = yes ]; then
		echo "WARN noiommu: enabling vfio unsafe noiommu mode"
		echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
		/usr/local/sbin/mana-vfio-bind '$bdf' || {
			echo "BIND_FAIL"
			exit 1
		}
	else
		echo "BIND_FAIL: vfio bind failed. Enable vIOMMU or set AZ_ALLOW_NOIOMMU=yes."
		exit 1
	fi
fi
systemd-run --unit=mana-watchdog --on-active=${WATCHDOG_MIN}min \\
	/bin/sh -c 'systemctl stop mana-guest.service 2>/dev/null; /usr/local/sbin/mana-vfio-unbind'
rm -f /root/mana-guest-serial.log
systemd-run --unit=mana-guest qemu-system-x86_64 -enable-kvm -cpu host \\
	-m $MEM_MB -smp 1 -no-reboot -display none \\
	-serial file:/root/mana-guest-serial.log -kernel /root/mana-kernel \\
	-append "netdev.ip=$HOST_IP/$PREFIXLEN:$GW" \\
	-device vfio-pci,host=$bdf
i=0
until grep -q 'TCP check server is ready' /root/mana-guest-serial.log 2>/dev/null; do
	i=\$((i+1))
	if [ \$i -gt $BOOT_TIMEOUT ]; then
		echo "BOOT_FAIL"
		exit 1
	fi
	sleep 1
done
echo "BOOT_PASS"
echo "TEST_WINDOW_OPEN \$(date -u +%FT%TZ)"
sleep $TEST_WINDOW
echo "TEST_WINDOW_CLOSE \$(date -u +%FT%TZ)"
EOS
chmod +x /root/mana-test.sh
systemctl stop mana-validation.service 2>/dev/null || true
systemctl stop mana-validation.timer 2>/dev/null || true
systemctl reset-failed mana-validation.service mana-guest.service \\
	mana-watchdog.service mana-watchdog.timer 2>/dev/null || true
rm -f /root/mana-test.log /root/mana-guest-serial.log
echo "== Start detached test driver =="
systemd-run --unit=mana-validation /bin/sh /root/mana-test.sh
echo "ARM_OK"
BODY_A_TAIL

cat > "$payload_b" <<PEER_EOF
#!/bin/sh
HOST_IP='$HOST_IP'
RETRIES=$PEER_RETRIES
OK_PING=no
OK_TCP=no
i=0
while [ \$i -lt \$RETRIES ]; do
	i=\$((i+1))
	if [ "\$OK_PING" = no ]; then
		if ping -c 3 -W 2 "\$HOST_IP" > /dev/null 2>&1; then
			echo "PING_PASS"
			OK_PING=yes
		fi
	fi
	if [ "\$OK_TCP" = no ]; then
		reply=\$(timeout 6 sh -c "exec 3<>/dev/tcp/\$HOST_IP/8080; head -c 8 <&3" 2>/dev/null || true)
		if [ "\$reply" = "mana-ok" ]; then
			echo "TCP_PASS"
			OK_TCP=yes
		fi
	fi
	if [ "\$OK_PING" = yes ] && [ "\$OK_TCP" = yes ]; then
		break
	fi
	sleep 5
done
if [ "\$OK_PING" = no ]; then
	echo "PING_FAIL"
fi
if [ "\$OK_TCP" = no ]; then
	echo "TCP_FAIL"
fi
echo "PEER_DONE ping=\$OK_PING tcp=\$OK_TCP"
PEER_EOF

cat > "$payload_c" <<'BODY_C_EOF'
#!/bin/sh
systemctl stop mana-guest.service 2>/dev/null || true
systemctl stop mana-watchdog.timer 2>/dev/null || true
systemctl stop mana-watchdog.service 2>/dev/null || true
if grep -q TEST_DONE /root/mana-test.log 2>/dev/null; then
	cat /root/mana-test.log
else
	echo "DRIVER_INCOMPLETE forcing restore"
	/usr/local/sbin/mana-vfio-unbind || true
	cat /root/mana-test.log 2>/dev/null || true
	echo "FORCED_RESTORE"
fi
BODY_C_EOF

run_host() {
	az vm run-command invoke --resource-group "$AZ_RG" \
		--name "$AZ_HOST_VM" --command-id RunShellScript \
		--scripts "@$1" --timeout "${2:-$CMD_TIMEOUT}" 2>&1 || true
}

run_peer() {
	az vm run-command invoke --resource-group "$AZ_RG" \
		--name "$AZ_PEER_VM" --command-id RunShellScript \
		--scripts "@$payload_b" --timeout "${1:-$CMD_TIMEOUT}" 2>&1 || true
}

echo "== Phase 1: upload the kernel and arm the host test driver =="
arm_out=$(run_host "$payload_a")
printf '%s\n' "$arm_out"
if ! printf '%s' "$arm_out" | grep -q ARM_OK; then
	fail "host refused to arm the test; see the output above"
fi

echo "== Phase 2: wait ${BOOT_WAIT}s for the nested guest to boot =="
sleep "$BOOT_WAIT"

echo "== Phase 3: probe the guest from the peer $AZ_PEER_VM =="
peer_out=$(run_peer "${AZ_PEER_CMD_TIMEOUT:-300}")
printf '%s\n' "$peer_out"

echo "== Phase 4: poll the host for the final report =="
deadline=$(( $(date +%s) + TEST_TIMEOUT ))
host_report=""
while :; do
	fetch_out=$(run_host "$payload_c" "${AZ_FETCH_TIMEOUT:-120}")
	if printf '%s' "$fetch_out" | grep -q 'TEST_DONE\|FORCED_RESTORE'; then
		host_report="$fetch_out"
		break
	fi
	if [[ $(date +%s) -ge $deadline ]]; then
		break
	fi
	echo "-- host still offline or driver still running; retry in ${POLL_INTERVAL}s"
	sleep "$POLL_INTERVAL"
done

if [[ -n "$host_report" ]]; then
	echo "== Host report =="
	printf '%s\n' "$host_report"
else
	echo "error: no final host report within ${TEST_TIMEOUT}s" >&2
	host_report="<missing>"
fi

count_in() {
	printf '%s' "$1" | grep -c -- "$2" || true
}

boot_pass=$(count_in "$host_report" "BOOT_PASS")
ping_pass=$(count_in "$peer_out" "PING_PASS")
tcp_pass=$(count_in "$peer_out" "TCP_PASS")
restore=$(count_in "$host_report" "RESTORE_PASS")
done_flag=$(count_in "$host_report" "TEST_DONE")

cat <<EOF

== Validation summary ==
BOOT_PASS=$boot_pass PING_PASS=$ping_pass TCP_PASS=$tcp_pass RESTORE_PASS=$restore TEST_DONE=$done_flag
EOF

if [[ "$boot_pass" -ge 1 && "$ping_pass" -ge 1 && "$tcp_pass" -ge 1 && "$done_flag" -ge 1 ]]; then
	echo "MANA_VALIDATION=PASS"
	trap - EXIT
	exit 0
fi

echo "MANA_VALIDATION=FAIL"
trap - EXIT
exit 1
