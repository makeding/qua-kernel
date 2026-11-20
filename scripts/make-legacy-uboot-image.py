#!/usr/bin/env python3
"""Adapt the ARM64 Image header for Qua legacy U-Boot and gzip it."""
import argparse
import gzip
from io import BytesIO
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('image', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()

image = bytearray(args.image.read_bytes())
if image[56:60] != b'ARMd':
    raise SystemExit('input is not an ARM64 Image')
text_offset = int.from_bytes(image[8:16], 'little')
if text_offset != 0:
    raise SystemExit(f'unexpected source text_offset: 0x{text_offset:x}')
image[8:16] = (0x20000000).to_bytes(8, 'little')
raw = BytesIO()
with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0,
                   compresslevel=9) as stream:
    stream.write(image)
args.output.write_bytes(raw.getvalue())
print('LEGACY_UBOOT_IMAGE_READY', args.output, args.output.stat().st_size)
