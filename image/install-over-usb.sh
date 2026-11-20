#!/bin/bash
# Host-side transport for the Qua RAM maintenance installer.
set -Eeuo pipefail
if [[ $# != 6 ]]; then
    echo 'usage: install-over-usb.sh RELEASE_DIR TARGET_BUNDLE ROOTFS.tar.zst AUTHORIZED_KEYS WPA_CONFIG STAGE2.tar.gz' >&2
    exit 2
fi
release=$(cd "$1" && pwd); bundle=$(cd "$2" && pwd)
archive=$3; keys=$4; wifi=$5; stage2=$6
serial=QuaMaintenance
for p in "$archive" "$keys" "$wifi" "$stage2"; do [[ -s "$p" ]] || { echo "missing $p" >&2; exit 1; }; done
expected=$(awk '$2 == "QuaStation-Armbian-trixie-arm64-rootfs.tar.zst" {print $1}' "$release/SHA256SUMS")
stage2_expected=$(awk '$2 == "QuaStation-maintenance-stage2.tar.gz" {print $1}' "$release/SHA256SUMS")
[[ "$expected" =~ ^[0-9a-f]{64}$ ]] || { echo 'missing rootfs checksum' >&2; exit 1; }
[[ "$stage2_expected" =~ ^[0-9a-f]{64}$ ]] || { echo 'missing stage2 checksum' >&2; exit 1; }
python3 - "$archive" "$expected" "$stage2" "$stage2_expected" <<'PY'
import hashlib, sys
for path, expected in ((sys.argv[1], sys.argv[2]), (sys.argv[3], sys.argv[4])):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while data := f.read(4 * 1024 * 1024): h.update(data)
    if h.hexdigest() != expected: raise SystemExit('checksum mismatch: ' + path)
PY
adb -s "$serial" get-state | grep -qx device
adb -s "$serial" shell -T '/bin/tar -xzf - -C /' < "$stage2"
adb -s "$serial" shell 'mkdir -p /run/qua-release /run/qua-bundle /run/qua-credentials'
cleanup() {
    adb -s "$serial" shell 'rm -rf /run/qua-release /run/qua-bundle /run/qua-credentials' >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
for name in install-target.sh install-factory-wifi-macs.py unzstd.py quastation-layout.mbr kernel-slot.bin dtb-slot.bin SHA256SUMS; do
    adb -s "$serial" push "$release/$name" "/run/qua-release/$name" >/dev/null
done
adb -s "$serial" push "$bundle/." /run/qua-bundle/ >/dev/null
adb -s "$serial" push "$keys" /run/qua-credentials/authorized_keys >/dev/null
adb -s "$serial" push "$wifi" /run/qua-credentials/wpa.conf >/dev/null
adb -s "$serial" shell 'chmod 600 /run/qua-credentials/*'
adb -s "$serial" shell -T 'bash /run/qua-release/install-target.sh /run/qua-bundle /run/qua-release - /run/qua-credentials/authorized_keys /run/qua-credentials/wpa.conf' < "$archive"
