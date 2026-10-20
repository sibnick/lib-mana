#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
revision=ec55ae17618feeb57c8c10109bcf5c42723e8e95
checksum=2814fb576f503f83e743ce41f2b56835e809f7df40ff01ffaef47f743308bb53
archive="$root/build/lib-lwip.tar.gz"
source="$root/build/lib-lwip"
stamp="$root/build/lib-lwip-prepared.sha256"

if [ -d "$source" ]; then
	if [ ! -f "$stamp" ]; then
		echo "error: existing lib-lwip tree has no preparation stamp: $source" >&2
		exit 1
	fi
	(cd "$root" && sha256sum -c "$stamp")
	exit 0
fi

mkdir -p "$root/build"
if [ ! -f "$archive" ]; then
	curl -fL --retry 3 \
		"https://codeload.github.com/unikraft/lib-lwip/tar.gz/$revision" \
		-o "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$root/build"
mv "$root/build/lib-lwip-$revision" "$source"
(cd "$root" && sha256sum build/lib-lwip.tar.gz) > "$stamp"
echo "prepared lib-lwip $revision at $source"
