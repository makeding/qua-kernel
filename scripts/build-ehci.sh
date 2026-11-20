#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	printf '%s\n' "usage: $0 /path/to/linux-7.3.4 /path/to/output" >&2
	exit 64
fi

tree=$1
out=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/scripts/install-kernel-overlay.sh" "$tree"

mkdir -p "$out"
cp "$root/configs/qua-sd-fs.config" "$out/.config"
make -C "$tree" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O="$out" olddefconfig
make -C "$tree" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O="$out" -j2 \
	Image realtek/rtd1295-quastation.dtb modules
