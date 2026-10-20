#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Provision the Azure test bed for MANA validation (ticket 50b980feba).
# The script creates billable resources. It stops unless AZ_CONFIRM=yes.
#
# Security posture: no public IP on the VMs and no inbound SSH port.
# One Standard public IP serves the NAT gateway for outbound traffic.
# The NSG allows only intra-subnet ICMP and intra-subnet TCP inbound.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: AZ_SUB=<subscription> AZ_LOC=<location> AZ_PREFIX=<prefix> \
       AZ_CONFIRM=yes scripts/azure_provision.sh

Required environment:
  AZ_SUB          Subscription name or ID.
  AZ_LOC          Location, for example westeurope.
  AZ_PREFIX       Name prefix. Lowercase letters and digits, 3 to 11 chars.
  AZ_CONFIRM      Must be yes. The run creates billable resources.

Optional environment, with defaults:
  AZ_RG           Resource group.            Default: <prefix>-rg
  AZ_VNET          Virtual network.          Default: <prefix>-vnet
  AZ_SUBNET        Subnet name.              Default: <prefix>-sub
  AZ_VNET_CIDR     VNet address prefix.      Default: 10.77.0.0/16
  AZ_SUBNET_CIDR   Subnet address prefix.    Default: 10.77.1.0/24
  AZ_NSG           Network security group.   Default: <prefix>-nsg
  AZ_SA            Boot diagnostics storage account. Default: <prefix>diag
  AZ_HOST_VM       Host VM name.             Default: <prefix>-host
  AZ_HOST_SIZE     Host VM size.             Default: Standard_D2ds_v5
  AZ_HOST_IP       Host static private IP.   Default: 10.77.1.10
  AZ_PEER_VM       Peer VM name.             Default: <prefix>-peer
  AZ_PEER_SIZE     Peer VM size.             Default: Standard_B2s
  AZ_PEER_IP       Peer static private IP.   Default: 10.77.1.11
  AZ_IMAGE         VM image.                 Default: Ubuntu2404
  AZ_ADMIN         Admin user name.          Default: azureuser

Notes:
  The host VM size must support nested virtualization and must be
  placed on MANA-capable hardware. See docs/azure-validation.md.
  If Azure rejects the storage account name, set AZ_SA to a new name.
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

[[ -n "${AZ_SUB:-}" && -n "${AZ_LOC:-}" && -n "${AZ_PREFIX:-}" ]] || {
	usage >&2
	fail "AZ_SUB, AZ_LOC, and AZ_PREFIX are required"
}
[[ "$AZ_PREFIX" =~ ^[a-z][a-z0-9]{2,10}$ ]] || fail "AZ_PREFIX must be lowercase letters and digits, 3 to 11 characters, starting with a letter"
[[ "${AZ_CONFIRM:-no}" == "yes" ]] || fail "AZ_CONFIRM must be yes: this script creates billable resources"

RG="${AZ_RG:-$AZ_PREFIX-rg}"
VNET="${AZ_VNET:-$AZ_PREFIX-vnet}"
SUBNET="${AZ_SUBNET:-$AZ_PREFIX-sub}"
VNET_CIDR="${AZ_VNET_CIDR:-10.77.0.0/16}"
SUBNET_CIDR="${AZ_SUBNET_CIDR:-10.77.1.0/24}"
NSG="${AZ_NSG:-$AZ_PREFIX-nsg}"
SA="${AZ_SA:-${AZ_PREFIX}diag}"
NAT="$AZ_PREFIX-nat"
NAT_IP="$AZ_PREFIX-natip"
HOST_VM="${AZ_HOST_VM:-$AZ_PREFIX-host}"
HOST_SIZE="${AZ_HOST_SIZE:-Standard_D2ds_v5}"
HOST_IP="${AZ_HOST_IP:-10.77.1.10}"
PEER_VM="${AZ_PEER_VM:-$AZ_PREFIX-peer}"
PEER_SIZE="${AZ_PEER_SIZE:-Standard_B2s}"
PEER_IP="${AZ_PEER_IP:-10.77.1.11}"
IMAGE="${AZ_IMAGE:-Ubuntu2404}"
ADMIN="${AZ_ADMIN:-azureuser}"

cleanup_summary() {
	cat <<EOF

Cleanup summary:
  az group delete --resource-group $RG --yes
  That one command removes the resource group and every billable
  resource listed above. No other resource survives this run.
EOF
}

echo "== Selecting subscription $AZ_SUB =="
az account set --subscription "$AZ_SUB"

echo "== Resource group $RG in $AZ_LOC =="
az group create --name "$RG" --location "$AZ_LOC" -o none

res_exists() {
	az "$@" --output none 2>/dev/null
}

echo "== Virtual network $VNET and subnet $SUBNET ($SUBNET_CIDR) =="
if res_exists network vnet show --resource-group "$RG" --name "$VNET"; then
	echo "   exists: vnet $VNET"
else
	az network vnet create --resource-group "$RG" --name "$VNET" \
		--address-prefixes "$VNET_CIDR" --subnet-name "$SUBNET" \
		--subnet-prefixes "$SUBNET_CIDR" -o none
fi

echo "== Network security group $NSG =="
if ! res_exists network nsg show --resource-group "$RG" --name "$NSG"; then
	az network nsg create --resource-group "$RG" --name "$NSG" -o none
fi
az network nsg rule create --resource-group "$RG" --nsg-name "$NSG" \
	--name allow-intra-icmp --priority 100 --access Allow \
	--protocol Icmp --direction Inbound \
	--source-address-prefixes "$SUBNET_CIDR" \
	--destination-port-ranges '*' -o none
az network nsg rule create --resource-group "$RG" --nsg-name "$NSG" \
	--name allow-intra-tcp --priority 110 --access Allow \
	--protocol Tcp --direction Inbound \
	--source-address-prefixes "$SUBNET_CIDR" \
	--destination-port-ranges '*' -o none

echo "== NAT gateway $NAT with Standard public IP $NAT_IP =="
if ! res_exists network public-ip show --resource-group "$RG" --name "$NAT_IP"; then
	az network public-ip create --resource-group "$RG" --name "$NAT_IP" \
		--sku Standard -o none
fi
if ! res_exists network nat gateway show --resource-group "$RG" --name "$NAT"; then
	az network nat gateway create --resource-group "$RG" --name "$NAT" \
		--public-ip-addresses "$NAT_IP" --idle-timeout 4 -o none
fi
az network vnet subnet update --resource-group "$RG" \
	--vnet-name "$VNET" --name "$SUBNET" \
	--network-security-group "$NSG" --nat-gateway "$NAT" -o none

echo "== Storage account $SA for boot diagnostics =="
if ! res_exists storage account show --resource-group "$RG" --name "$SA"; then
	az storage account create --resource-group "$RG" --name "$SA" \
		--location "$AZ_LOC" --sku Standard_LRS \
		--allow-blob-public-access false -o none
fi

echo "== Host VM $HOST_VM ($HOST_SIZE, $HOST_IP) =="
if ! res_exists vm show --resource-group "$RG" --name "$HOST_VM"; then
	az vm create --resource-group "$RG" --name "$HOST_VM" \
		--location "$AZ_LOC" --size "$HOST_SIZE" --image "$IMAGE" \
		--vnet-name "$VNET" --subnet "$SUBNET" \
		--private-ip-address "$HOST_IP" \
		--public-ip-address '' --nsg '' \
		--admin-username "$ADMIN" --generate-ssh-keys \
		--storage-sku Standard_LRS --boot-diagnostics-storage "$SA" -o none
fi

echo "== Peer VM $PEER_VM ($PEER_SIZE, $PEER_IP) =="
if ! res_exists vm show --resource-group "$RG" --name "$PEER_VM"; then
	az vm create --resource-group "$RG" --name "$PEER_VM" \
		--location "$AZ_LOC" --size "$PEER_SIZE" --image "$IMAGE" \
		--vnet-name "$VNET" --subnet "$SUBNET" \
		--private-ip-address "$PEER_IP" \
		--public-ip-address '' --nsg '' \
		--admin-username "$ADMIN" --generate-ssh-keys \
		--storage-sku Standard_LRS -o none
fi

host_ip=$(az vm list-ip-addresses --resource-group "$RG" --name "$HOST_VM" \
	--query '[].virtualMachine.network.privateIpAddresses[0]' -o tsv)
peer_ip=$(az vm list-ip-addresses --resource-group "$RG" --name "$PEER_VM" \
	--query '[].virtualMachine.network.privateIpAddresses[0]' -o tsv)

cat <<EOF

Created resources:
  resource group          $RG
  virtual network         $VNET (subnet $SUBNET, $SUBNET_CIDR)
  NSG                     $NSG (inbound: intra-subnet ICMP, intra-subnet TCP)
  NAT gateway             $NAT (public IP $NAT_IP, outbound only)
  storage account         $SA (boot diagnostics, serial console logs)
  host VM                 $HOST_VM ($HOST_SIZE) private IP $host_ip
  peer VM                 $PEER_VM ($PEER_SIZE) private IP $peer_ip

Next steps: see docs/azure-validation.md.
EOF
cleanup_summary
