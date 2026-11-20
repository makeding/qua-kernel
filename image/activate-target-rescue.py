#!/usr/bin/env python3
"""Install the verified per-device RAM rescue slot and arm both env banks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('bundle', type=Path)
parser.add_argument('--rootfs', type=Path, required=True,
                    help='mounted mmcblk0p2 Btrfs @ subvolume')
args = parser.parse_args()
bundle = args.bundle.resolve()
plan = json.loads((bundle / 'plan.json').read_text())
manifest = json.loads((bundle / 'manifest.json').read_text())
spec = importlib.util.spec_from_file_location('qua_install_helpers', bundle / 'install_helpers.py')
helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
sha = lambda data: hashlib.sha256(data).hexdigest()
if Path('/sys/class/block/mmcblk0/device/cid').read_text().strip().lower() != plan['cid']:
    raise SystemExit('target CID differs from backup')
root = args.rootfs.resolve()
source = subprocess.run(['findmnt', '-n', '-o', 'SOURCE', '--target', str(root)],
                        check=True, capture_output=True, text=True).stdout.strip()
if source != '/dev/mmcblk0p2[/@]':
    raise SystemExit('rootfs must be the target eMMC Btrfs @ subvolume')
for name in ('root/.ssh/authorized_keys', 'etc/wpa_supplicant/wpa_supplicant-wlan0.conf',
             'lib/modules/' + plan['kernelrelease'] + '/modules.dep'):
    path = root / name
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit('prepared rootfs lacks ' + name)
for name, path in (('kernel', root / 'boot/Image-legacy-uboot.gz'),
                   ('dtb', root / 'boot/rtd1295-quastation.dtb')):
    data = path.read_bytes(); size = plan['buffers'][name]['bytes']
    if len(data) > size or sha(data + bytes(size - len(data))) != plan['buffers'][name]['sha256']:
        raise SystemExit('prepared rootfs boot content differs: ' + name)
slot = plan['trial_slot']; size = slot['sectors'] * 512; offset = slot['start_sector'] * 512
if slot['start_sector'] != 0x33800 or size != 0x40000:
    raise SystemExit('unsupported rescue slot')
for protected in plan['protected_ranges']:
    if slot['start_sector'] < protected['end'] and protected['start'] < slot['start_sector'] + slot['sectors']:
        raise SystemExit('rescue slot overlaps ' + protected['name'])
payload = (bundle / 'trial-slot.bin').read_bytes()
if len(payload) != size or sha(payload) != manifest['trial_slot_sha256']:
    raise SystemExit('rescue bundle payload mismatch')
candidate_count = (manifest['candidate_bytes'] + 511) // 512
commands = [f'mmc read 0x25000000 33800 {candidate_count:x}',
    'mmc read 0x24000000 338f0 100', 'mmc read 0x01500000 28125 800',
    'mmc read 0x26000000 30000 3000', 'mmc read 0x23000000 33f00 1f',
    'mmc read 0x01b00000 13781 19a9', 'mmc read 0x01f00000 339fe 1',
    'mmc read 0x01f02000 339ff 1', 'mw.l 0x98007680 0xa5 1',
    'mw.l 0x980076c0 0x800000 1', 'mw.l 0x9800768c 0xc11e7a00 1',
    'mw.l 0x98007684 1 1', 'mw.l 0x98007680 0xff 1',
    'env set bootcmd ${qua_root_stage2}', 'go a', 'dcache off', 'icache off',
    'go 0x01f00000', 'b2ndbc']
bootcmd = ';'.join(commands).encode()
if len(bootcmd) >= 1024:
    raise SystemExit('rescue boot command exceeds environment limit')
def save(name, data):
    path = bundle / name
    if path.exists():
        if path.read_bytes() != data: raise RuntimeError('existing backup differs: ' + name)
    else:
        helper.save_file(path, data)
def interrupted(signum, _): raise RuntimeError('interrupted by signal ' + str(signum))
for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT): signal.signal(sig, interrupted)
fd = os.open('/dev/mmcblk0', os.O_RDWR)
old_slot = None; envs = []; changed = False
try:
    if sha(os.pread(fd, 733664, 0x20800)) != plan['original_bootcode_sha256']:
        raise RuntimeError('unsupported first-stage bootcode')
    if sha(os.pread(fd, 1048576, 0x5024a00)) != plan['buffers']['stage2']['sha256']:
        raise RuntimeError('unsupported second-stage bootcode')
    for name in ('kernel', 'dtb', 'audio'):
        item = plan['buffers'][name]
        if sha(os.pread(fd, item['bytes'], item['lba'] * 512)) != item['sha256']:
            raise RuntimeError('boot content mismatch: ' + name)
    old_slot = os.pread(fd, size, offset)
    if sha(old_slot) != plan['buffers']['trial_slot_before']['sha256']:
        raise RuntimeError('rescue slot differs from target backup')
    save('slot-before.bin', old_slot)
    save('factory-before.bin', os.pread(fd, 0x400000, 0x220000))
    expected_env = plan['buffers']['base_env']['sha256']
    base_offset = (slot['base_env_sector'] - slot['start_sector']) * 512
    if sha(payload[base_offset:base_offset + 0x20000]) != expected_env:
        raise RuntimeError('embedded target environment mismatch')
    for bank in (0x220000, 0x420000):
        helper.FACTORY_OFFSET = bank
        pos, raw, pairs, end = helper.read_env(fd)
        if sha(raw) != expected_env:
            raise RuntimeError('factory environment changed since backup')
        env = dict(pairs)
        if env.get(b'bootcmd') != plan['normal']['bootcmd'].encode() or b'bootargs' in env:
            raise RuntimeError('unsupported factory environment state')
        save('environment-before-arm-' + hex(bank) + '.bin', raw)
        stage2_cmd = b'env set fdt_high ffffffffffffffff;booti 0x26000000 - 0x23000000'
        updates = {b'bootcmd': bootcmd, b'bootdelay': b'2', b'qua_root_stage2': stage2_cmd}
        new_pairs = [(k, updates.get(k, v)) for k, v in pairs]
        if b'bootdelay' not in env: new_pairs.append((b'bootdelay', b'2'))
        if b'qua_root_stage2' not in env: new_pairs.append((b'qua_root_stage2', stage2_cmd))
        envs.append((pos, raw, helper.encode_env(raw, new_pairs, end)))
    directory = os.open(bundle, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(directory)
    finally: os.close(directory)
    changed = True
    helper.pwrite_verified(fd, offset, payload)
    for pos, _, updated in envs: helper.pwrite_verified(fd, pos, updated)
    print('RESCUE_SLOT_AND_BOTH_ENV_BANKS_VERIFIED', flush=True)
except BaseException:
    if changed:
        for pos, raw, _ in envs: helper.pwrite_verified(fd, pos, raw)
        helper.pwrite_verified(fd, offset, old_slot)
        print('RESCUE_AND_ENV_RESTORED', flush=True)
    raise
finally:
    os.close(fd)
