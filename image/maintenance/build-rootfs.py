#!/usr/bin/env python3
"""Assemble a credential-free arm64 maintenance initramfs tree."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

p = argparse.ArgumentParser()
p.add_argument('public_root', type=Path)
p.add_argument('modules', type=Path)
p.add_argument('busybox', type=Path)
p.add_argument('usb_exec', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--bootstrap', action='store_true', help='only the USB ADB first stage')
a = p.parse_args()
src, out = a.public_root.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
for name in ('bin', 'dev', 'proc', 'sys', 'run', 'etc', 'root', 'mnt',
             'usr/bin', 'usr/sbin', 'usr/lib/android-sdk/platform-tools',
             'usr/lib/aarch64-linux-gnu', 'lib'):
    (out / name).mkdir(parents=True, exist_ok=True)
for name in ('init',):
    shutil.copy2(Path(__file__).with_name(name), out / name)
shutil.copy2(a.busybox, out / 'bin/busybox')
shutil.copy2(a.usb_exec, out / 'bin/usb-only-exec')
for f in (out / 'init', out / 'bin/busybox', out / 'bin/usb-only-exec'):
    f.chmod(0o755)
os.symlink('busybox', out / 'bin/sh')
if not a.bootstrap:
    for rel in Path(__file__).with_name('python-imports.txt').read_text().splitlines():
        if not rel or rel.startswith('/') or '..' in Path(rel).parts:
            raise RuntimeError('invalid Python runtime path: ' + rel)
        source = src / 'usr/lib/python3.13' / rel
        target = out / 'usr/lib/python3.13' / rel
        if rel == 'sitecustomize.py' and not source.exists():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
with tarfile.open(a.modules, 'r:gz') as archive:
    archive.extractall(out / 'lib/modules', filter='data')
bins = ['usr/lib/android-sdk/platform-tools/adbd']
if not a.bootstrap:
    bins += ['usr/bin/python3.13', 'usr/bin/bash', 'usr/bin/tar', 'usr/bin/findmnt',
             'usr/bin/mountpoint', 'usr/sbin/blockdev', 'usr/bin/btrfs', 'usr/sbin/mkfs.btrfs']
queue = []
def copy_file(rel):
    target = src / rel
    if not target.exists(): raise RuntimeError('missing image input: ' + rel)
    dest = out / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Copy resolved contents so absolute source symlinks cannot escape the image.
    shutil.copy2(target.resolve(), dest)
    queue.append(dest)
for name in bins: copy_file(name)
if not a.bootstrap:
    copy_file('usr/lib/aarch64-linux-gnu/libzstd.so.1')
    os.symlink('python3.13', out / 'usr/bin/python3')
    os.symlink('../usr/bin/bash', out / 'bin/bash')
    queue.extend((out / 'usr/lib/python3.13').rglob('*.so'))
queue.extend([out / 'bin/busybox', out / 'bin/usb-only-exec'])
seen = set()
while queue:
    f = queue.pop()
    if f in seen: continue
    seen.add(f)
    if f.read_bytes()[:4] != b'\x7fELF': continue
    headers = subprocess.run(['readelf', '-l', '-d', str(f)], check=True,
                             stdout=subprocess.PIPE, text=True).stdout
    for soname in re.findall(r'\(NEEDED\).*?\[(.*?)\]', headers):
        dirs = ('usr/lib/aarch64-linux-gnu', 'usr/lib/aarch64-linux-gnu/android')
        if any((out / d / soname).exists() for d in dirs): continue
        for d in dirs:
            if (src / d / soname).exists():
                copy_file(d + '/' + soname)
                break
        else:
            raise RuntimeError('missing shared library: ' + soname)
    for interp in re.findall(r'Requesting program interpreter: (.*?)\]', headers):
        rel = interp.lstrip('/')
        dest = out / rel
        if not dest.exists(): copy_file(rel)
(out / 'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
(out / 'etc/group').write_text('root:x:0:\n')
print('MAINTENANCE_ROOTFS_READY', out)
