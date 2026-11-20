#!/usr/bin/env python3
"""Wrap an xz initramfs in the legacy ARM64 ramdisk header used by Qua."""
import argparse
from pathlib import Path
import struct
import zlib

p = argparse.ArgumentParser()
p.add_argument('initramfs', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
payload = a.initramfs.read_bytes()
if not payload.startswith(b'\xfd7zXZ\x00'):
    raise SystemExit('expected an xz initramfs')
name = b'Qua maintenance initramfs'.ljust(32, b'\0')
header = struct.pack('>7I4B32s', 0x27051956, 0, 0, len(payload), 0, 0,
                     zlib.crc32(payload), 5, 22, 3, 0, name)
crc = zlib.crc32(header)
header = header[:4] + struct.pack('>I', crc) + header[8:]
with a.output.open('xb') as stream:
    stream.write(header)
    stream.write(payload)
print('LEGACY_RAMDISK_READY', a.output, len(payload))
