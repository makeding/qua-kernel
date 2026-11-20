#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
	printf '%s\n' "usage: install-kernel-overlay.sh /path/to/linux-7.3.4" >&2
	exit 64
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tree=$1
cp -Rp "$root/kernel/." "$tree/"
dtb_makefile="$tree/arch/arm64/boot/dts/realtek/Makefile"
dtb_target='dtb-$(CONFIG_ARCH_REALTEK) += rtd1295-quastation.dtb'
if ! grep -Fxq "$dtb_target" "$dtb_makefile"; then
	printf '%s\n' "$dtb_target" >> "$dtb_makefile"
fi
install -D -m 0644 "$root/dts/rtd1295-quastation.dts" \
	"$tree/arch/arm64/boot/dts/realtek/rtd1295-quastation.dts"
