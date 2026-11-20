#!/bin/bash
# Run only from the RAM maintenance system, never from the target rootfs.
set -Eeuo pipefail
if [[ $# != 5 ]]; then
    echo 'usage: install-target.sh BUNDLE RELEASE ROOTFS.tar.zst|- AUTHORIZED_KEYS WPA_CONFIG' >&2
    exit 2
fi
bundle=$(realpath "$1"); release=$(realpath "$2"); archive=$3
[[ "$archive" == - ]] || archive=$(realpath "$archive")
keys=$(realpath "$4"); wifi=$(realpath "$5")
root=/mnt/qua-install
[[ $(uname -r) =~ ^[0-9]+(\.[0-9]+)+-huggy-[0-9]+$ ]] || { echo 'expected Qua maintenance kernel' >&2; exit 1; }
[[ -s "$keys" && -s "$wifi" ]]
expected=$(awk '$2 == "QuaStation-Armbian-trixie-arm64-rootfs.tar.zst" {print $1}' "$release/SHA256SUMS")
[[ "$expected" =~ ^[0-9a-f]{64}$ ]] || { echo 'missing rootfs archive checksum' >&2; exit 1; }
if [[ "$archive" != - ]]; then
    [[ -s "$archive" && $(sha256sum "$archive" | cut -d' ' -f1) == "$expected" ]] || {
        echo 'rootfs archive checksum mismatch' >&2; exit 1;
    }
fi
inputs=("$bundle" "$release" "$keys" "$wifi")
[[ "$archive" == - ]] || inputs+=("$archive")
for path in "${inputs[@]}"; do
    source=$(findmnt -n -o SOURCE --target "$path")
    [[ "$source" != /dev/mmcblk0* ]] || { echo 'installer inputs must be outside target eMMC' >&2; exit 1; }
done
for part in /dev/mmcblk0p1 /dev/mmcblk0p2 /dev/mmcblk0p3; do
    if findmnt -rn -S "$part" | grep -q .; then echo "$part is mounted" >&2; exit 1; fi
done
mountpoint -q "$root" && { echo "$root is already mounted" >&2; exit 1; }
mkdir -p "$root"
cleanup() { mountpoint -q "$root" && umount "$root" || true; }
trap cleanup EXIT INT TERM
python3 "$bundle/repartition-target.py" "$bundle" "$release/quastation-layout.mbr"
blockdev --rereadpt /dev/mmcblk0
if command -v udevadm >/dev/null; then udevadm settle; fi
for _ in {1..50}; do [[ -b /dev/mmcblk0p2 ]] && break; sleep 0.1; done
[[ -b /dev/mmcblk0p2 ]] || { echo 'new root partition did not appear' >&2; exit 1; }
mkfs.btrfs -f -L QUA_ROOT /dev/mmcblk0p2
mount /dev/mmcblk0p2 "$root"
btrfs subvolume create "$root/@"
umount "$root"
mount -o subvol=@,compress=zstd:3 /dev/mmcblk0p2 "$root"
python3 "$release/unzstd.py" "$archive" "$expected" | /usr/bin/tar --xattrs --acls --numeric-owner -xpf - -C "$root"
install -d -m 700 "$root/root/.ssh"
install -m 600 "$keys" "$root/root/.ssh/authorized_keys"
install -d -m 700 "$root/etc/wpa_supplicant"
install -m 600 "$wifi" "$root/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
python3 "$release/install-factory-wifi-macs.py" --rootfs "$root"
sync
python3 "$bundle/write-boot-slots.py" "$bundle" "$release"
python3 "$bundle/activate-target-rescue.py" "$bundle" --rootfs "$root"
sync
echo 'INSTALL_COMPLETE; reboot into the installed system'
