# Qua Station install image

The release target is an Armbian Trixie arm64 root filesystem with the Qua
kernel, matching modules, firmware and video runtime. The public image must
not contain the developer's Wi-Fi credentials, SSH authorized keys, host keys,
machine identity or device-specific factory environment.

The image is installed to the eMMC root partition after the device-specific
rescue handoff is prepared. A generic disk-wide `dd` image is not valid: it
would overwrite the original firmware and per-device factory data. Preserve
the original boot chain and factory environment. Capture and retain the
target's own backups before changing boot slots or partitions.

The current accepted Qua layout keeps the existing firmware prefix and stores
the Linux kernel, DTB and rescue payload in their designated raw slots. The
root filesystem is Btrfs subvolume `@` on `mmcblk0p2`. Do not format the SD
card or unrelated eMMC partitions. Exact device-specific installation
commands and boot acceptance remain to be completed and tested before this
can be described as a ready-to-flash release.

The image's local USB ADB starts after the boot console releases the gadget.
It is confined to Unix and netlink sockets so it cannot listen on TCP. USB
enumeration, reconnect and sustained transfers must be verified on the actual
board before publishing this image; building the root filesystem alone does
not establish that they work.

The installer below is for a maintenance system running from RAM. The host
sends the compressed rootfs through local USB ADB directly into the target
filesystem; no SD card or rootfs-sized target-side staging file is required.
The small release files, target-specific bundle and connection credentials
are copied into RAM first. The installer destroys the factory contents
of eMMC partitions 1–3. It is under development and has not yet been validated
end to end on a factory device; do not use it as a release installation guide.

The first initramfs boots with all eMMC partitions unmounted and starts USB
ADB with network socket families blocked. The host then transfers the small
maintenance tools archive into RAM through ADB, followed by the streamed
rootfs. Maintenance boot and installation remain unverified on hardware.

During installation, each target's CRC-checked factory environment supplies
its Wi-Fi addresses. The installer writes per-device link files from those
values. Missing or invalid addresses leave the existing link configuration
unchanged; no device's addresses are baked into the public image.

With the RAM maintenance system connected as `QuaMaintenance`, capture the
target's original eMMC and its CID on the host before changing the disk:

```sh
set -o pipefail
adb -s QuaMaintenance shell 'cat /sys/class/block/mmcblk0/device/cid' > cid.txt
adb -s QuaMaintenance exec-out 'dd if=/dev/mmcblk0 bs=1048576 2>/dev/null' | gzip -1 > factory-emmc.img.gz
python3 RELEASE_DIR/prepare-target-bundle.py factory-emmc.img.gz "$(cat cid.txt)" RELEASE_DIR TARGET_BUNDLE
```

Keep the backup and generated bundle: they contain device-specific data.
After preparing the bundle, the intended host-side command is:

```sh
./install-over-usb.sh RELEASE_DIR TARGET_BUNDLE ROOTFS.tar.zst AUTHORIZED_KEYS WPA_CONFIG STAGE2.tar.gz
```

It verifies the rootfs archive on the host, copies only the small installer
and credentials into RAM, and streams the archive through ADB. The target
checks the stream hash before arming rescue. This command is not yet validated
end to end on hardware. It requires `adb`, Python 3, the host's SSH public
key, and a wpa_supplicant configuration for the target network.

The Alpine edge aarch64 `adbd` package was also checked on the actual board.
Its dynamic closure needs only `libbase`, `libcutils` and `liblog` from the
larger ART package, plus musl, libstdc++, libgcc, libbsd and libmd. However,
this particular binary attempted to bind TCP port 5038 and exited when network
sockets were prohibited. USB FunctionFS operation was not demonstrated, so
the maintenance image currently keeps the separately verified Debian adbd.
