#!/usr/bin/env python3
"""Construct a rescue installation bundle from a target's own eMMC backup."""
import argparse
import hashlib
import gzip
import io
import json
from pathlib import Path
import re
import shutil
import struct
import tarfile
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('backup', type=Path, help='raw or gzip-compressed eMMC backup from this device')
parser.add_argument('cid', help='CID of the same target eMMC')
parser.add_argument('release', type=Path, help='downloaded public release directory')
parser.add_argument('output', type=Path, help='new private output directory outside the target eMMC')
args = parser.parse_args()
release = args.release.resolve()
profile = json.loads((release / 'profile.json').read_text())
manifest = json.loads((release / 'rescue-package/manifest.json').read_text())
layout = json.loads(Path(__file__).with_name('boot-slot-layout.json').read_text())
if not profile.get('device_specific_environment'):
    raise SystemExit('rescue is not configured for a per-device environment')
if not re.fullmatch('[0-9a-fA-F]{32}', args.cid):
    raise SystemExit('CID must be 32 hexadecimal characters')
expected_size = 15269888 * 512
compressed = args.backup.name.endswith('.gz')
if not compressed and args.backup.stat().st_size != expected_size:
    raise SystemExit(f'unsupported eMMC size: {args.backup.stat().st_size}')
if compressed:
    total = 0
    with gzip.open(args.backup, 'rb') as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            total += len(chunk)
            if total > expected_size: raise SystemExit('backup exceeds the supported eMMC size')
    if total != expected_size: raise SystemExit(f'unsupported decompressed eMMC size: {total}')

def digest(b): return hashlib.sha256(b).hexdigest()
def read_at(f, offset, size):
    f.seek(offset); b = f.read(size)
    if len(b) != size: raise RuntimeError('short backup read')
    return b
def environment(blob):
    with tarfile.open(fileobj=io.BytesIO(blob)) as archive:
        entries = [m for m in archive.getmembers() if m.name == 'tmp/factory/env.txt']
        if len(entries) != 1 or entries[0].size != 131072 or not entries[0].isfile():
            raise RuntimeError('factory environment member mismatch')
        raw = archive.extractfile(entries[0]).read()
    if struct.unpack_from('<I', raw)[0] != zlib.crc32(raw[4:]):
        raise RuntimeError('factory environment CRC mismatch')
    end = raw[4:].find(b'\0\0')
    if end < 0: raise RuntimeError('environment terminator absent')
    pairs = [x.split(b'=', 1) for x in raw[4:4 + end].split(b'\0')]
    if any(len(x) != 2 for x in pairs) or len({x[0] for x in pairs}) != len(pairs):
        raise RuntimeError('malformed environment')
    return raw, dict(pairs)

with (gzip.open(args.backup, 'rb') if compressed else args.backup.open('rb')) as f:
    mbr = read_at(f, 0, 512)
    if mbr[510:512] != b'\x55\xaa':
        raise SystemExit('unsupported partition table signature')
    if digest(mbr) != layout['mbr_sha256']:
        raise SystemExit('unsupported Qua partition table')
    expected_parts = [(0x83, 196608, 3041264), (0x83, 3237888, 9615344),
                      (0x83, 12853248, 819184), (0x05, 13672448, 1269760)]
    for i, (kind, start, sectors) in enumerate(expected_parts):
        entry = mbr[446 + i * 16:462 + i * 16]
        if (entry[4], *struct.unpack_from('<II', entry, 8)) != (kind, start, sectors):
            raise SystemExit(f'unsupported factory partition layout at partition {i + 1}')
    if digest(mbr) != '80d10f852cda01e082d8d20728b0d6bea486c5dde89a510c37bb0728ef8daa9d':
        raise SystemExit('factory MBR differs from the verified install baseline')
    profile['original_mbr_sha256'] = digest(mbr)
    bootcode_spec = layout['first_stage']
    stage2_spec = layout['second_stage']
    bootcode = read_at(f, bootcode_spec['offset'], bootcode_spec['bytes'])
    stage2 = read_at(f, stage2_spec['offset'], stage2_spec['bytes'])
    if digest(bootcode) != bootcode_spec['sha256']:
        raise SystemExit('unsupported stock first-stage bootcode')
    if digest(stage2) != stage2_spec['sha256'] or \
            stage2_spec['sha256'] != profile['buffers']['stage2']['sha256']:
        raise SystemExit('unsupported stock second-stage bootcode')
    envs = [environment(read_at(f, bank, 0x400000)) for bank in (0x220000, 0x420000)]
    if envs[0][0] != envs[1][0]:
        raise SystemExit('factory banks differ; automatic installation is unsupported')
    raw, env = envs[0]
    if b'bootcmd' not in env or b'bootargs' in env:
        raise SystemExit('unsupported factory boot environment')
    try:
        factory_bootcmd = env[b'bootcmd'].decode('ascii')
    except UnicodeDecodeError:
        raise SystemExit('factory boot command is not ASCII')
    if not re.fullmatch(r'\s*env set bootargs androidboot\.bootloader=\$\{ver\};\s*bootr\s*', factory_bootcmd):
        raise SystemExit('unsupported stock boot command')
    normal = dict(profile['normal'])
    normal['bootcmd'] = factory_bootcmd
    normal['initial_bootargs_absent'] = True
    profile['normal'] = normal
    profile['cid'] = args.cid.lower()
    profile['disk'] = '/dev/mmcblk0'
    profile['original_bootcode_sha256'] = digest(bootcode)
    profile['protected_ranges'] = [
        {'name': 'normal kernel', 'start': 196608, 'end': 208896},
        {'name': 'diagnostic DTB', 'start': 208896, 'end': 208911},
        {'name': 'native DTB', 'start': 212736, 'end': 212767},
        {'name': 'probe restore', 'start': 212800, 'end': 212928},
        {'name': 'maintenance initrd', 'start': 212992, 'end': 228526},
    ]
    slot = profile['trial_slot']; start = slot['start_sector']
    before = read_at(f, start * 512, slot['sectors'] * 512)
    profile['buffers']['trial_slot_before'] = {'bytes': len(before), 'sha256': digest(before)}
    profile['original_boot_slots'] = {}
    for name in ('kernel', 'dtb'):
        slot = layout['slots'][name]
        sectors, lba = slot['sectors'], slot['lba']
        data = (release / (name + '-slot.bin')).read_bytes()
        if len(data) != sectors * 512 or digest(data) != profile['buffers'][name]['sha256']:
            raise SystemExit('release artifact hash mismatch: ' + name)
        profile['buffers'][name].update(lba=lba, sectors=sectors)
        old_data = read_at(f, lba * 512, len(data))
        profile['original_boot_slots'][name] = {'lba': lba, 'bytes': len(old_data),
                                                 'sha256': digest(old_data)}
    audio = read_at(f, 0x13781 * 512, 0x19a9 * 512)
    profile['buffers']['audio'] = {'lba': 0x13781, 'bytes': len(audio), 'sha256': digest(audio)}
    profile['buffers']['base_env'] = {'bytes': len(raw), 'sha256': digest(raw),
                                      'crc32': f'{zlib.crc32(raw):08x}'}

candidate = (release / 'rescue-package/u-boot-ram-console.bin').read_bytes()
if len(candidate) != manifest['candidate_bytes'] or digest(candidate) != manifest['candidate_sha256']:
    raise SystemExit('rescue binary hash mismatch')
loader = (release / 'rescue-package/chain-copy-arm32.bin').read_bytes()
stub = (release / 'chain-to-arm64.bin').read_bytes()
if digest(loader) != manifest['trampoline_sha256']:
    raise SystemExit('rescue loader hash mismatch')
if digest(stub) != '4702193b0750355ea86240c21f187f7be85422aeabf3897b01e7ace0da3cbfcf':
    raise SystemExit('stage2 trampoline hash mismatch')
if len(loader) > 512 or len(stub) > 512 or len(candidate) > slot['candidate_max_sectors'] * 512:
    raise SystemExit('rescue payload exceeds the slot')
args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
payload = bytearray(slot['sectors'] * 512)
for data, sector in ((candidate, start), (raw, slot['base_env_sector']),
                     (loader, slot['first_loader_sector']), (stub, slot['second_loader_sector'])):
    offset = (sector - start) * 512; payload[offset:offset + len(data)] = data
(args.output / 'plan.json').write_text(json.dumps(profile, indent=2) + '\n')
(args.output / 'trial-slot.bin').write_bytes(payload)
manifest['trial_slot_sha256'] = digest(payload)
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
for name in ('activate-target-rescue.py', 'write-boot-slots.py',
             'repartition-target.py', 'install_helpers.py'):
    shutil.copy2(release / name, args.output / name)
print('TARGET_RESCUE_BUNDLE_READY', args.output, 'nonce=' + profile['nonce'])
