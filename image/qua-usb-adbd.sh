#!/bin/bash
# Own both FunctionFS and adbd so a restart cleans up the entire gadget.
set -Eeuo pipefail
g=/sys/kernel/config/usb_gadget/qua-adb
ffs=/dev/usb-ffs/adb
udc=dwc2.2.auto
pid=
owned=0
cleanup() {
    trap - EXIT INT TERM
    [[ "$owned" == 1 ]] || return 0
    [[ ! -e "$g/UDC" ]] || printf '\n' > "$g/UDC" || true
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    mountpoint -q "$ffs" && umount "$ffs" || true
    rm -f "$g/configs/c.1/ffs.adb"
    rmdir "$g/functions/ffs.adb" "$g/configs/c.1/strings/0x409" \
        "$g/configs/c.1" "$g/strings/0x409" "$g" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM
test -d "/sys/class/udc/$udc"
test ! -e "$g"
systemctl stop serial-getty@ttyGS0.service || true
if grep -q '^g_serial ' /proc/modules; then modprobe -r g_serial; fi
modprobe usb_f_fs
mkdir "$g"
owned=1
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" "$g/functions/ffs.adb" "$ffs"
printf '0x18d1\n' > "$g/idVendor"
printf '0x4ee7\n' > "$g/idProduct"
printf 'Qua Station\n' > "$g/strings/0x409/manufacturer"
printf 'Qua USB ADB\n' > "$g/strings/0x409/product"
printf 'QuaStation\n' > "$g/strings/0x409/serialnumber"
printf 'ADB\n' > "$g/configs/c.1/strings/0x409/configuration"
ln -s "$g/functions/ffs.adb" "$g/configs/c.1/ffs.adb"
mount -t functionfs adb "$ffs"
/usr/lib/android-sdk/platform-tools/adbd &
pid=$!
bound=0
for attempt in {1..50}; do
    kill -0 "$pid"
    if printf '%s\n' "$udc" > "$g/UDC" 2>/dev/null; then bound=1; break; fi
    sleep 0.1
done
[[ "$bound" == 1 ]] || { echo 'USB ADB: gadget bind failed' >&2; exit 1; }
wait "$pid"
