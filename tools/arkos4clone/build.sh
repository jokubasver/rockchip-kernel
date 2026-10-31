#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build the ArkOS4Clone kernel and lay it out like the OS repository
# (see README.md in this directory).
set -euo pipefail

usage() {
	cat <<USAGE
Usage: $0 [--os DIR] [--out DIR]
  --os DIR   ArkOS4Clone OS repository whose console folders pick the DTBs
             (default: ../arkos4clone next to this repository)
  --out DIR  build and package directory (default: out/arkos4clone)
Environment: CROSS_COMPILE (default aarch64-linux-gnu-), JOBS (default nproc),
             LOCALVERSION (appended to the release, default empty)
USAGE
}

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$repo/out/arkos4clone"
os="$repo/../arkos4clone"
while [ $# -gt 0 ]; do
	case "$1" in
	--os) os="${2:?--os needs a directory}"; shift 2 ;;
	--out) out="${2:?--out needs a directory}"; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
cross="${CROSS_COMPILE:-aarch64-linux-gnu-}"
jobs="${JOBS:-$(nproc)}"
consoles="$os/boot/dArkOS/consoles"
if [ ! -d "$consoles" ]; then
	echo "$consoles not found: pass the OS repository with --os" >&2
	exit 2
fi
consoles="$(realpath "$consoles")"
mkdir -p "$out"
out="$(realpath "$out")"
build="$out/build"
# dist mirrors the OS repository (boot/dArkOS, rootfs/dArkOS) and holds
# nothing else; extra keeps the files the OS does not ship.
dist="$out/dist"
extra="$out/extra"

cd "$repo"
# Setting LOCALVERSION, even empty, stops setlocalversion from appending "+",
# so the release is exactly <version>-arkos4clone$LOCALVERSION: the OS module
# directory is named after it.
make_args=(O="$build" ARCH=arm64 CROSS_COMPILE="$cross" LOCALVERSION="${LOCALVERSION-}")
make "${make_args[@]}" clone_defconfig
make "${make_args[@]}" -j"$jobs" Image modules dtbs
release="$(cat "$build/include/config/kernel.release")"
dts="$build/arch/arm64/boot/dts/rockchip"

# The OS ships this kernel next to its 4.4 kernel: everything for 5.10 goes
# in a 5.10 folder beside the 4.4 file it stands in for.
rm -rf "$dist" "$extra"
boot="$dist/boot/dArkOS/consoles"
mkdir -p "$boot/kernel/5.10" "$boot/dtbo/5.10" "$dist/rootfs/dArkOS/usr" \
	"$extra/dtbs"
cp "$build/arch/arm64/boot/Image" "$boot/kernel/5.10/Image"
cp "$dts/rk3326-oc-voltage.dtbo" "$boot/dtbo/5.10/"
cp "$dts"/rk3326-*.dtb "$extra/dtbs/"

# A console's 5.10 DTB has the same name as its 4.4 one.
mapped=0
for dir in "$consoles"/*/; do
	name="$(basename "$dir")"
	for dtb in "$dir"*.dtb; do
		[ -e "$dtb" ] || continue
		src="$dts/$(basename "$dtb")"
		if [ ! -f "$src" ]; then
			echo "$name: $(basename "$dtb") is not built by this kernel" >&2
			exit 1
		fi
		mkdir -p "$boot/$name/5.10"
		cp "$src" "$boot/$name/5.10/"
		mapped=$((mapped + 1))
	done
done
if [ "$mapped" -eq 0 ]; then
	echo "no console DTBs found in $consoles" >&2
	exit 1
fi

make "${make_args[@]}" INSTALL_MOD_PATH="$dist/rootfs/dArkOS/usr" \
	INSTALL_MOD_STRIP=1 modules_install
modules="$dist/rootfs/dArkOS/usr/lib/modules"
rm -f "$modules/$release/build" "$modules/$release/source"

epoch="$(git show -s --format=%ct HEAD)"
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
	-C "$modules" -cf - "$release" | gzip -n > "$extra/modules-$release.tar.gz"
cp "$build/.config" "$extra/kernel.config"
cp "$build/System.map" "$extra/System.map"

dirty=false
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	dirty=true
	echo "warning: uncommitted changes are part of this build" >&2
fi
python3 - "$dist" "$extra" "$release" "$(git rev-parse HEAD)" "$dirty" "$mapped" \
	"$("${cross}gcc" --version | head -n 1)" <<'PY'
import json
import sys
from pathlib import Path

dist, extra, release, commit, dirty, mapped, compiler = sys.argv[1:]
dist = Path(dist)
extra = Path(extra)
manifest = {
    "kernel_release": release,
    "commit": commit,
    "uncommitted_changes": dirty == "true",
    "compiler": compiler,
    "rk3326_dtbs": len(list((extra / "dtbs").glob("*.dtb"))),
    "console_dtbs": int(mapped),
    "modules": len(list((dist / "rootfs/dArkOS/usr/lib/modules" / release).rglob("*.ko"))),
}
(extra / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
(cd "$dist" && find . -type f -printf '%P\0' | LC_ALL=C sort -z |
	xargs -0 sha256sum) > "$out/SHA256SUMS.tmp"
mv "$out/SHA256SUMS.tmp" "$extra/SHA256SUMS"

echo "Release: $release"
echo "OS tree: $dist"
cat "$extra/build.json"
