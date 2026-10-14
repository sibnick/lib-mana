#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
revision=e31b2c44effe583eb8910e713fda814b6e049213
checksum=aba55cd0d3bac5997574a0e63dbcdcf739910838753eb272a00854cef1cb4cc5
archive="$root/build/unikraft.tar.gz"
source="$root/build/unikraft"
patchfile="$root/patches/unikraft-e31b2c44-msix.patch"
stamp="$root/build/unikraft-prepared.sha256"

if [ -d "$source" ]; then
	if [ ! -f "$stamp" ]; then
		echo "error: existing Unikraft tree has no preparation stamp: $source" >&2
		exit 1
	fi
	(cd "$root" && sha256sum -c "$stamp")
	patch --dry-run -R -p1 -d "$source" < "$patchfile"
	exit 0
fi

mkdir -p "$root/build"
if [ ! -f "$archive" ]; then
	curl -fL --retry 3 \
		"https://codeload.github.com/unikraft/unikraft/tar.gz/$revision" \
		-o "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
tar -xzf "$archive" -C "$root/build"
mv "$root/build/unikraft-$revision" "$source"
patch --batch -p1 -d "$source" < "$patchfile"
(cd "$root" && sha256sum patches/unikraft-e31b2c44-msix.patch) > "$stamp"
