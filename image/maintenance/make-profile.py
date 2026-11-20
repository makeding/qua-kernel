#!/usr/bin/env python3
"""Create a RAM-only rescue profile for the maintenance initrd."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

p = argparse.ArgumentParser()
p.add_argument('release_profile', type=Path)
p.add_argument('initrd', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
profile = json.loads(a.release_profile.read_text())
data = a.initrd.read_bytes()
if not data.startswith(bytes.fromhex('27051956')) or len(data) > 0x4000000:
    raise SystemExit('unexpected or oversized legacy ramdisk')
profile['buffers']['initrd'] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                                'crc32': f'{zlib.crc32(data):08x}'}
profile['nonce'] = hashlib.sha256(bytes.fromhex(profile['buffers']['kernel']['sha256']) +
                                   bytes.fromhex(profile['buffers']['dtb']['sha256']) +
                                   bytes.fromhex(profile['buffers']['initrd']['sha256'])).hexdigest()[:16]
profile['normal']['bootargs'] += ' rdinit=/init'
a.output.write_text(json.dumps(profile, indent=2) + '\n')
print('MAINTENANCE_PROFILE_READY', profile['nonce'])
