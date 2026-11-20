#!/usr/bin/env python3
"""Activate the installed Qua kernel package through the verified slot writer."""
import hashlib
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
layout = json.loads((root / 'boot-slot-layout.json').read_text())
profile = json.loads((root / 'rescue-profile.json').read_text())
release = profile['kernelrelease']
package = 'linux-image-' + release
version = subprocess.run(['dpkg-query', '-W', '-f=${Version}', package],
                         check=True, capture_output=True, text=True).stdout.strip()
root_source = subprocess.run(['findmnt', '-n', '-o', 'SOURCE', '--target', '/'],
                             check=True, capture_output=True, text=True).stdout.strip()
if root_source != '/dev/mmcblk0p2[/@]':
    raise SystemExit('Qua kernel slot update requires the eMMC Btrfs @ rootfs')
mounted = subprocess.run(['findmnt', '-rn', '-S', '/dev/mmcblk0p1'],
                         check=False, capture_output=True, text=True)
if mounted.stdout.strip():
    raise SystemExit('Qua boot-cache partition is mounted')

def sha(data):
    return hashlib.sha256(data).hexdigest()

sector = layout['sector_bytes']
slots = layout['slots']
new = {name: (root / (name + '-slot.bin')).read_bytes()
       for name in ('kernel', 'dtb')}
for name in ('kernel', 'dtb'):
    slot = slots[name]
    expected = profile['buffers'][name]
    if len(new[name]) != slot['sectors'] * sector or sha(new[name]) != expected['sha256']:
        raise SystemExit('package slot payload mismatch: ' + name)
image = gzip.decompress(Path('/boot/Image-legacy-uboot.gz').read_bytes())
if image[56:60] != b'ARMd' or int.from_bytes(image[8:16], 'little') != 0x20000000:
    raise SystemExit('package kernel is not compatible with Qua legacy U-Boot')
slot_names = ('kernel', 'dtb')
initrd_path = Path('/boot/QuaStation-maintenance-initramfs.uImage')
if initrd_path.is_file():
    initrd = initrd_path.read_bytes()
    initrd_capacity = slots['initrd']['sectors'] * sector
    if not initrd.startswith(bytes.fromhex('27051956')) or len(initrd) > initrd_capacity:
        raise SystemExit('maintenance initramfs does not fit the Qua RAM slot')
    new['initrd'] = initrd + bytes(initrd_capacity - len(initrd))
    slot_names += ('initrd',)

fd = os.open('/dev/mmcblk0', os.O_RDONLY)
try:
    if os.pread(fd, 512, 0) != (root / 'quastation-layout.mbr').read_bytes():
        raise SystemExit('unsupported Qua eMMC partition table')
    first = layout['first_stage']
    if sha(os.pread(fd, first['bytes'], first['offset'])) != first['sha256']:
        raise SystemExit('unsupported Qua first-stage bootcode')
    second = layout['second_stage']
    if sha(os.pread(fd, second['bytes'], second['offset'])) != second['sha256']:
        raise SystemExit('unsupported Qua second-stage bootcode')
    current = {}
    for name in slot_names:
        slot = slots[name]
        current[name] = os.pread(fd, len(new[name]), slot['lba'] * sector)
        if len(current[name]) != len(new[name]):
            raise SystemExit('short read from Qua ' + name + ' boot slot')
finally:
    os.close(fd)

if all(current[name] == new[name] for name in slot_names):
    print('QUA_PACKAGE_BOOT_SLOTS_ALREADY_CURRENT', release, version)
    raise SystemExit(0)

bundle = Path('/var/lib/qua-kernel/boot-slot-backups') / version
if bundle.exists():
    raise SystemExit('boot-slot backup exists for this package version; refusing to replace it')
bundle.mkdir(mode=0o700, parents=True)
for name in slot_names:
    (bundle / (name + '-slot.bin')).write_bytes(new[name])
buffers = {}
for name in slot_names:
    metadata = dict(profile['buffers'][name]) if name in profile['buffers'] else {
        'bytes': len(new[name]), 'sha256': sha(new[name])}
    buffers[name] = dict(metadata, **slots[name])
plan = {
    'cid': Path('/sys/class/block/mmcblk0/device/cid').read_text().strip().lower(),
    'original_bootcode_sha256': layout['first_stage']['sha256'],
    'buffers': {
        **buffers,
        'stage2': dict(layout['second_stage']),
    },
    'original_boot_slots': {
        name: {'lba': slots[name]['lba'], 'bytes': len(current[name]),
               'sha256': sha(current[name])}
        for name in slot_names
    },
}
(bundle / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
subprocess.run([sys.executable, str(root / 'write-boot-slots.py'),
                str(bundle), str(bundle)], check=True)
print('QUA_PACKAGE_BOOT_SLOTS_INSTALLED', release, version)
