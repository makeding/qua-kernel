#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build"
out="$build/output"
release=$(cat "$out/include/config/kernel.release")
package_root="$out/debian/linux-image-$release"
version=$(sed -n 's/^Version: //p' "$package_root/DEBIAN/control")
image_deb="$build/linux-image-${release}_${version}_arm64.deb"
public="$out/maintenance-public-root"
modules="$out/maintenance-modules-extract"
stage="$out/maintenance-rootfs"
module_archive="$out/kernel-modules.tar.gz"
cpio_image="$build/QuaStation-maintenance-initramfs.cpio.xz"
uimage="$build/QuaStation-maintenance-initramfs.uImage"
usb_exec="$out/usb-only-exec"

test -s "$image_deb"
test -s "$usb_exec"
test ! -e "$public"
test ! -e "$modules"
test ! -e "$stage"
test ! -e "$cpio_image"
test ! -e "$uimage"

cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	python3 -c 'import shutil,sys; [shutil.rmtree(p, ignore_errors=True) for p in sys.argv[1:]]' \
		"$public" "$modules" "$stage"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$public" "$modules"
tar -xzf - -C "$public"
dpkg-deb -x "$image_deb" "$modules"
tar -czf "$module_archive" -C "$modules/lib/modules" "$release"
python3 "$root/image/maintenance/build-rootfs.py" "$public" "$module_archive" \
	"$public/usr/bin/busybox" "$usb_exec" "$stage" --bootstrap
size_kib=$(du -sk "$public" "$modules" "$stage" | awk '{total += $1} END {print total}')
test "$size_kib" -le 1048576
(cd "$stage" && find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --owner=0:0) \
	| xz -C crc32 -9e -T1 -c > "$cpio_image"
python3 "$root/image/maintenance/make-uimage.py" "$cpio_image" "$uimage"
slot_bytes=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["slots"]["initrd"]["sectors"] * d["sector_bytes"])' \
	"$root/image/boot-slot-layout.json")
uimage_bytes=$(wc -c < "$uimage")
test "$uimage_bytes" -le "$slot_bytes" || {
	echo "maintenance uImage exceeds verified initrd slot ($uimage_bytes > $slot_bytes)" >&2
	exit 1
}
ls -lh "$cpio_image" "$uimage"
