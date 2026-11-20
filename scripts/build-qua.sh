#!/bin/sh
set -eu
if [ "$#" -ne 2 ]; then
  printf '%s\n' 'usage: build-qua.sh /path/to/linux-7.3.4 /absolute/path/to/output' >&2
  exit 64
fi
tree=$(CDPATH= cd -- "$1" && pwd)
mkdir -p "$2"
out=$(CDPATH= cd -- "$2" && pwd)
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/scripts/install-kernel-overlay.sh" "$tree"
cp "$root/configs/qua-sd-fs.config" "$out/.config"
if [ -n "${QUA_LOCALVERSION:-}" ]; then
  "$tree/scripts/config" --file "$out/.config" --set-str LOCALVERSION "$QUA_LOCALVERSION"
fi
"$tree/scripts/config" --file "$out/.config" --set-str EXTRA_FIRMWARE_DIR "$root/firmware" \
  --module ATA --module SATA_AHCI_PLATFORM --module AHCI_QUASTATION \
  --module GPIO_QUASTATION_ISO_GLUE --module USB_DWC2_QUASTATION_GLUE \
  --enable USB_DWC2_DUAL_ROLE \
  --enable NEW_LEDS --enable LEDS_CLASS --enable LEDS_GPIO
jobs=$(nproc)
make -C "$tree" ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" O="$out" olddefconfig
make -C "$tree" ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" O="$out" -j"$jobs" \
  Image realtek/rtd1295-quastation.dtb modules
${CC:-${CROSS_COMPILE:-aarch64-linux-gnu-}gcc} -static -Os \
  -o "$out/usb-only-exec" "$root/image/maintenance/usb-only-exec.c"
release=$(cat "$out/include/config/kernel.release")
cp "$out/arch/arm64/boot/Image" "$out/Image"
cp "$out/arch/arm64/boot/dts/realtek/rtd1295-quastation.dtb" "$out/rtd1295-quastation.dtb"
printf '%s\n' "$release" > "$out/kernelrelease"
python3 "$root/scripts/make-legacy-uboot-image.py" "$out/Image" "$out/Image-legacy-uboot.gz"
python3 "$root/image/make-rescue-profile.py" "$out/Image-legacy-uboot.gz" \
  "$out/rtd1295-quastation.dtb" "$out/rescue-profile.json"
for module in gpu gpu/i2c gpu/dma-limit video/ve1 video/ion; do
  mkdir -p "$out/qua-modules/$module"
  make -C "$tree" ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" \
    O="$out" -j"$jobs" M="$root/modules/$module" MO="$out/qua-modules/$module" modules
done
