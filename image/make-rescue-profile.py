#!/usr/bin/env python3
"""Generate a public rescue profile from the official image boot artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('kernel', type=Path, help='legacy U-Boot-compatible Image.gz')
parser.add_argument('dtb', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
kernel, dtb = args.kernel.read_bytes(), args.dtb.read_bytes()
kernelrelease = (args.kernel.parent / 'kernelrelease').read_text().strip()
if not kernelrelease.startswith('7.3.4-huggy-'):
    raise SystemExit('unexpected kernel release')
layout = json.loads(Path(__file__).with_name('boot-slot-layout.json').read_text())
kernel_capacity = layout['slots']['kernel']['sectors'] * layout['sector_bytes']
dtb_sectors = layout['slots']['dtb']['sectors']
if not kernel or len(kernel) > kernel_capacity or not dtb or len(dtb) > dtb_sectors * 512:
    raise SystemExit('boot artifact exceeds the verified slot')
kernel += bytes(kernel_capacity - len(kernel))
dtb += bytes(dtb_sectors * 512 - len(dtb))
nonce = hashlib.sha256(kernel + dtb).hexdigest()[:16]
def record(data):
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'crc32': f'{zlib.crc32(data):08x}'}
plan = {
    'nonce': nonce,
    'kernelrelease': kernelrelease,
    'device_specific_environment': True,
    'normal': {'bootargs': 'console=ttyS0,115200 earlycon=uart8250,mmio32,0x98007800 '
        'loglevel=8 net.ifnames=0 root=/dev/mmcblk0p2 rootfstype=btrfs '
        'rootflags=subvol=@,compress=zstd:3,nodiscard rootwait rw clk_ignore_unused '
        'rtl8192ee.aspm=0 rtl8192ee.ips=0 rtl8192ee.fwlps=0 rtl8192ee.swlps=0 '
        'rtl8192ee.msi=0 rtl8192ee.debug_level=5 rtl8192ee.debug_mask=7'},
    'buffers': {'kernel': record(kernel), 'dtb': record(dtb),
                'stage2': {'bytes': 1048576, 'crc32': '33727ec5',
                           'sha256': 'ef69d76e331043dc25b4848def2522d7cc8cd82d49219abcaa4066f895eeaeae'},
                'base_env': {'bytes': 131072, 'crc32': '00000000'}},
    'boot_slots': layout['slots'],
    'trial_slot': {'start_sector': 210944, 'sectors': 512, 'base_env_sector': 211184,
                   'first_loader_sector': 211454, 'second_loader_sector': 211455,
                   'candidate_max_sectors': 240},
}
args.output.parent.joinpath('kernel-slot.bin').write_bytes(kernel)
args.output.parent.joinpath('dtb-slot.bin').write_bytes(dtb)
args.output.write_text(json.dumps(plan, indent=2) + '\n')
print('PUBLIC_RESCUE_PROFILE', nonce)
