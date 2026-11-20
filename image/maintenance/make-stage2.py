#!/usr/bin/env python3
"""Package only files absent from the running USB bootstrap initramfs."""
import argparse
from pathlib import Path
import tarfile

p = argparse.ArgumentParser()
p.add_argument('bootstrap', type=Path)
p.add_argument('maintenance', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
base, full = a.bootstrap.resolve(), a.maintenance.resolve()
with tarfile.open(a.output, 'x:gz', format=tarfile.GNU_FORMAT) as archive:
    for item in sorted(full.rglob('*')):
        rel = item.relative_to(full)
        existing = base / rel
        if existing.exists() or existing.is_symlink():
            if item.is_file() and not item.is_symlink() and item.read_bytes() != existing.read_bytes():
                raise RuntimeError('bootstrap and maintenance disagree: ' + str(rel))
            continue
        archive.add(item, arcname=str(rel), recursive=False)
print('MAINTENANCE_STAGE2_READY', a.output)
