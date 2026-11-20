#!/usr/bin/env python3
"""Write and read back the two boot slots after verifying the target backup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal

parser = argparse.ArgumentParser()
parser.add_argument('bundle', type=Path)
parser.add_argument('release', type=Path)
args = parser.parse_args()
bundle, release = args.bundle.resolve(), args.release.resolve()
plan = json.loads((bundle / 'plan.json').read_text())
if Path('/sys/class/block/mmcblk0/device/cid').read_text().strip().lower() != plan['cid']:
    raise SystemExit('target CID differs from backup')
def sha(data): return hashlib.sha256(data).hexdigest()
def save(name, data):
    path = bundle / name
    if path.exists():
        if path.read_bytes() != data: raise RuntimeError('backup mismatch: ' + name)
        return
    with path.open('xb') as f:
        os.fchmod(f.fileno(), 0o600); f.write(data); f.flush(); os.fsync(f.fileno())
def write(fd, offset, data):
    if os.pwrite(fd, data, offset) != len(data): raise RuntimeError('short write')
    os.fsync(fd)
    if os.pread(fd, len(data), offset) != data: raise RuntimeError('readback mismatch')
def interrupted(signum, _frame): raise RuntimeError('interrupted by signal ' + str(signum))
for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
fd = os.open('/dev/mmcblk0', os.O_RDWR)
original = {}; written = False
try:
    if sha(os.pread(fd, 733664, 0x20800)) != plan['original_bootcode_sha256']:
        raise RuntimeError('first-stage bootcode mismatch')
    if sha(os.pread(fd, 1048576, 0x5024a00)) != plan['buffers']['stage2']['sha256']:
        raise RuntimeError('second-stage bootcode mismatch')
    slot_names = ('kernel', 'dtb')
    if 'initrd' in plan['buffers'] and 'initrd' in plan['original_boot_slots']:
        slot_names += ('initrd',)
    new = {}
    for name in slot_names:
        item = plan['buffers'][name]; old = plan['original_boot_slots'][name]
        new[name] = (release / (name + '-slot.bin')).read_bytes()
        if len(new[name]) != item['bytes'] or sha(new[name]) != item['sha256']:
            raise RuntimeError('release slot mismatch: ' + name)
        original[name] = os.pread(fd, old['bytes'], old['lba'] * 512)
        if sha(original[name]) != old['sha256']:
            raise RuntimeError('current slot differs from target backup: ' + name)
        save('boot-' + name + '-before.bin', original[name])
    written = True
    for name in slot_names:
        write(fd, plan['buffers'][name]['lba'] * 512, new[name])
    print('BOOT_SLOTS_WRITTEN_AND_VERIFIED', flush=True)
except BaseException:
    if written:
        for name in slot_names:
            write(fd, plan['buffers'][name]['lba'] * 512, original[name])
        print('BOOT_SLOTS_RESTORED', flush=True)
    raise
finally:
    os.close(fd)
