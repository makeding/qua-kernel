#!/usr/bin/env python3
"""Make an offline RAM-only trial for the existing Qua USB rescue console."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

p = argparse.ArgumentParser()
p.add_argument('release', type=Path)
p.add_argument('resident_template', type=Path)
p.add_argument('watchdog', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
release = a.release.resolve()
profile = json.loads((release / 'maintenance-profile.json').read_text())
image = (release / 'u-boot-maintenance.bin').read_bytes()
template = a.resident_template.read_bytes()
watchdog = a.watchdog.read_bytes()
placeholder = struct.pack('<5I', 0x26000000, 0, 0, 0x21000, 0x51554254)
if hashlib.sha256(template).hexdigest() != 'd125b4043de0ddc650563fb98e7957e277880b3c3a9a24950dd4d31174002b7e' or \
        template.count(placeholder) != 1 or struct.pack('<I', 0x2bb58) not in template:
    raise SystemExit('unsupported resident USB shutdown trampoline')
if hashlib.sha256(watchdog).hexdigest() != 'eeaa4beb364405143f5ded8dca2f623435de3321da1c83ad55e58dafef610953':
    raise SystemExit('unsupported watchdog helper')
if ('qua_ram_chain=' + profile['nonce']).encode() not in image:
    raise SystemExit('maintenance rescue does not match profile')
files = {
    'watchdog.bin': watchdog,
    'Image-legacy-uboot.gz': (release / 'kernel-slot.bin').read_bytes(),
    'board.dtb': (release / 'dtb-slot.bin').read_bytes(),
    'maintenance.uImage': (release / 'QuaStation-maintenance-initramfs.uImage').read_bytes(),
    'u-boot.bin': image,
    'chain-copy-usb-sd.bin': template.replace(placeholder,
        struct.pack('<5I', 0x25000000, len(image), zlib.crc32(image), 0x21000, 0x51554254)),
}
for name, key in (('Image-legacy-uboot.gz', 'kernel'), ('board.dtb', 'dtb'),
                  ('maintenance.uImage', 'initrd')):
    data, expected = files[name], profile['buffers'][key]
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit('profile mismatch: ' + name)
a.output.mkdir(parents=True, exist_ok=False)
for name, data in files.items(): (a.output / name).write_bytes(data)
manifest = {'status': 'offline-built-not-device-accepted', 'nonce': profile['nonce'],
            'resident_rescue_usb_shutdown': '0x2bb58',
            'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                      for name, data in files.items()}}
(a.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('MAINTENANCE_USB_TRIAL_READY', a.output)
