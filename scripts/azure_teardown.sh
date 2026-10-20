#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Delete the MANA validation test bed. This removes billable resources,
# including any running VM. The script stops unless AZ_CONFIRM=yes.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: AZ_RG=<resource group> AZ_CONFIRM=yes scripts/azure_teardown.sh

Required environment:
  AZ_RG        Resource group created by scripts/azure_provision.sh.
  AZ_CONFIRM   Must be yes. The run deletes billable resources.

Optional environment:
  AZ_CMD_TIMEOUT  Azure CLI timeout in seconds. Default: 1800
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

[[ -n "${AZ_RG:-}" ]] || {
	usage >&2
	fail "AZ_RG is required"
}
[[ "${AZ_CONFIRM:-no}" == "yes" ]] || fail "AZ_CONFIRM must be yes: this script deletes billable resources"

echo "== Resources that will be deleted =="
az resource list --resource-group "$AZ_RG" \
	--query '[].{type:type, name:name}' -o table

echo "== Deleting resource group $AZ_RG =="
az group delete --name "$AZ_RG" --yes \
	--timeout "${AZ_CMD_TIMEOUT:-1800}"

echo "== Verifying deletion =="
if az group show --name "$AZ_RG" -o none 2>/dev/null; then
	fail "resource group $AZ_RG still exists after delete"
fi

cat <<EOF

Cleanup summary:
  Resource group $AZ_RG is deleted. VMs, the network interface,
  the NSG, the NAT gateway, the Standard public IP, the virtual
  network, the disks, and the storage account are gone. Billing for
  these resources stops now. Nothing from the test bed survives.
EOF
