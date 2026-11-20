#!/usr/bin/env python3
"""Install the accepted Qua partition layout; original firmware stays intact."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import struct

parser = argparse.ArgumentParser()
parser.add_argument('bundle', type=Path)
parser.add_argument('layout', type=Path)
args = parser.parse_args()
bundle = args.bundle.resolve(); plan = json.loads((bundle / 'plan.json').read_text())
if Path('/sys/class/block/mmcblk0/device/cid').read_text().strip().lower() != plan['cid']:
    raise SystemExit('target CID differs from backup')
if not re.fullmatch(r'\d+(?:\.\d+)+-huggy-\d+', os.uname().release):
    raise SystemExit('boot the verified maintenance kernel before repartitioning')
mounts = Path('/proc/self/mountinfo').read_text()
if any('/dev/mmcblk0p' + str(n) + ' ' in mounts for n in (1, 2, 3)):
    raise SystemExit('factory partitions must be unmounted')
layout = args.layout.read_bytes()
if len(layout) != 512 or hashlib.sha256(layout).hexdigest() != '63ade7ecfe7bfa1d99796f6e7f692dc0d2d366c57623f9632bc7b2e8c9a41bd0':
    raise SystemExit('partition layout artifact mismatch')
fd = os.open('/dev/mmcblk0', os.O_RDWR)
try:
    original = os.pread(fd, 512, 0)
    if hashlib.sha256(original).hexdigest() != plan['original_mbr_sha256']:
        raise RuntimeError('current MBR differs from verified target backup')
    if original[:446] != layout[:446]:
        raise RuntimeError('layout would alter firmware prefix')
    backup = bundle / 'mbr-before.bin'
    with backup.open('xb') as f:
        os.fchmod(f.fileno(), 0o600); f.write(original); f.flush(); os.fsync(f.fileno())
    try:
        if os.pwrite(fd, layout, 0) != len(layout): raise RuntimeError('short MBR write')
        os.fsync(fd)
        if os.pread(fd, 512, 0) != layout: raise RuntimeError('MBR readback mismatch')
    except BaseException:
        if os.pwrite(fd, original, 0) != len(original): raise RuntimeError('MBR rollback failed')
        os.fsync(fd)
        if os.pread(fd, 512, 0) != original: raise RuntimeError('MBR rollback readback failed')
        raise
finally:
    os.close(fd)
print('MBR_WRITTEN_AND_VERIFIED; run partprobe or blockdev --rereadpt')
