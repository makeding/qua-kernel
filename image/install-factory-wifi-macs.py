#!/usr/bin/env python3
"""Restore per-device Wi-Fi MAC addresses from the factory U-Boot environment."""
import argparse
import io
import os
from pathlib import Path
import re
import struct
import tarfile
import zlib

p = argparse.ArgumentParser()
p.add_argument('--rootfs', type=Path, default=Path('/'))
p.add_argument('--disk', type=Path, default=Path('/dev/mmcblk0'))
a = p.parse_args()
fd = os.open(a.disk, os.O_RDONLY)
try:
    data = os.pread(fd, 0x400000, 0x220000)
finally:
    os.close(fd)
if len(data) != 0x400000:
    raise SystemExit('short factory environment read')
with tarfile.open(fileobj=io.BytesIO(data)) as archive:
    entries = [x for x in archive.getmembers() if x.name == 'tmp/factory/env.txt']
    if len(entries) != 1 or not entries[0].isfile() or entries[0].size != 0x20000:
        raise SystemExit('unexpected factory environment')
    raw = archive.extractfile(entries[0]).read()
if len(raw) != 0x20000 or struct.unpack_from('<I', raw)[0] != zlib.crc32(raw[4:]):
    raise SystemExit('invalid factory environment CRC')
end = raw[4:].find(b'\0\0')
if end < 0: raise SystemExit('invalid factory environment terminator')
env = {}
for item in raw[4:4 + end].split(b'\0'):
    key, sep, value = item.partition(b'=')
    if not sep or key in env: raise SystemExit('malformed factory environment')
    env[key] = value
directory = a.rootfs / 'etc/systemd/network'
directory.mkdir(parents=True, exist_ok=True)
for name, driver, path in (
    ('wlan0', 'rtl8192ee', 'platform-9804e000.pcie-pci-0000:00:00.0'),
    ('wlan1', 'rtl8821ae', 'platform-9803b000.pcie-pci-0001:01:00.0'),
):
    value = env.get((name + ' hwaddr').encode(), b'').decode('ascii', 'replace').lower()
    compact = value.replace(':', '')
    if not re.fullmatch('[0-9a-f]{12}', compact):
        print('FACTORY_WIFI_MAC_MISSING', name)
        continue
    mac = bytes.fromhex(compact)
    if mac == bytes(6) or mac == b'\xff' * 6 or mac[0] & 1:
        print('FACTORY_WIFI_MAC_INVALID', name)
        continue
    address = ':'.join(f'{x:02x}' for x in mac)
    link = directory / ('10-qua-' + name + '.link')
    link.write_text(f'[Match]\nDriver={driver}\nPath={path}\n\n'
                    f'[Link]\nMACAddressPolicy=none\nMACAddress={address}\n')
    print('FACTORY_WIFI_MAC_CONFIGURED', name, address)
