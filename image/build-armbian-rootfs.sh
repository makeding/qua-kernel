#!/bin/bash
# Run as root on the Linux release builder, using the official Armbian cache.
set -Eeuo pipefail
archive=$(realpath "${1:?usage: build-armbian-rootfs.sh ARMBIAN.tar.zst KERNEL.deb OUTPUT}")
deb=$(realpath "${2:?}")
out=$(realpath -m "${3:?}")
project=$(cd "$(dirname "$0")/.." && pwd)
root="$out/rootfs"
test ! -e "$root"
mkdir -p "$root"
cleanup() { rm -rf -- "$root"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
tar --xattrs --acls --numeric-owner -xpf "$archive" -C "$root"
grep -qx 'VERSION_ID="13"' "$root/etc/os-release"
test -f "$root/etc/armbian-release"
# Extract the Debian package without running its hardware-facing postinst on CI.
dpkg-deb -x "$deb" "$root"
release=$(dpkg-deb -f "$deb" Package)
release=${release#linux-image-}
depmod -b "$root" "$release"
install -d "$root/lib/firmware/rtlwifi" "$root/etc/systemd/network" \
  "$root/etc/ssh/sshd_config.d" "$root/etc/wpa_supplicant" \
  "$root/etc/systemd/system/multi-user.target.wants"
install -m 644 "$project"/firmware/rtlwifi/*.bin "$root/lib/firmware/rtlwifi/"
printf '%s\n' 'LABEL=QUA_ROOT / btrfs subvol=@,compress=zstd:3,noatime,nodiscard 0 1' > "$root/etc/fstab"
printf 'qua\n' > "$root/etc/hostname"
printf '%s\n' 'PasswordAuthentication no' 'KbdInteractiveAuthentication no' \
  'PermitRootLogin prohibit-password' > "$root/etc/ssh/sshd_config.d/10-quastation.conf"
printf '[Match]\nName=wlan0\n[Link]\nActivationPolicy=manual\nRequiredForOnline=no\n[Network]\nDHCP=yes\n' > "$root/etc/systemd/network/20-wlan0.network"
for service in systemd-networkd ssh wpa_supplicant@wlan0; do
  unit=${service/@wlan0/@}
  ln -sfn "/usr/lib/systemd/system/$unit.service" "$root/etc/systemd/system/multi-user.target.wants/$service.service"
done
install -d "$root/usr/lib/armbian" "$root/etc/systemd/system/sshd-keygen.service.d"
cp "$root/etc/armbian-release" "$root/usr/lib/armbian/armbian-release.upstream"
sed -i -e 's/^BOARD=.*/BOARD=quastation/' \
  -e 's/^BOARD_NAME=.*/BOARD_NAME="KDDI Qua Station"/' \
  -e 's/^BOARDFAMILY=.*/BOARDFAMILY=rtd1295/' \
  -e 's/^LINUXFAMILY=.*/LINUXFAMILY=quastation-mainline/' "$root/etc/armbian-release"
printf '[Unit]\nConditionFirstBoot=\n' > "$root/etc/systemd/system/sshd-keygen.service.d/quastation.conf"
rm -f "$root"/etc/ssh/ssh_host_* "$root/root/.ssh/authorized_keys"
: > "$root/etc/machine-id"
rm -f "$root/etc/systemd/system/basic.target.wants/armbian-resize-filesystem.service"
tar --xattrs --acls --numeric-owner -C "$root" -cpf - . \
  | zstd -T0 -o "$out/QuaStation-Armbian-trixie-arm64-rootfs.tar.zst"
