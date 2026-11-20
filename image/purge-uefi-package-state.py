#!/usr/bin/env python3
"""Remove only the UEFI board kernel packages from an extracted rootfs.

This operates on dpkg metadata as files.  It never invokes an ARM64 package
manager or a maintainer script on the build host.
"""
from pathlib import Path
import json
import sys

root = Path(sys.argv[1])
removed = {
    "armbian-bsp-cli-uefi-arm64-current-grub",
    "linux-dtb-current-arm64",
    "linux-image-current-arm64",
}
status = root / "var/lib/dpkg/status"
paragraphs = status.read_text().split("\n\n")
kept, found = [], []
for paragraph in paragraphs:
    package = next((line[9:] for line in paragraph.splitlines()
                    if line.startswith("Package: ")), None)
    if package in removed:
        found.append(package)
    else:
        kept.append(paragraph)
if set(found) != removed:
    raise SystemExit(f"unexpected installed package state: found={found}")
status.write_text("\n\n".join(kept).rstrip() + "\n")

info = root / "var/lib/dpkg/info"
deleted_info = []
for package in sorted(removed):
    for path in info.glob(f"{package}.*"):
        path.unlink()
        deleted_info.append(str(path.relative_to(root)))

deleted_hooks = []
for relative in (
    "etc/kernel/postinst.d/zz-update-grub",
    "etc/kernel/postrm.d/zz-update-grub",
):
    path = root / relative
    if path.exists() or path.is_symlink():
        path.unlink()
        deleted_hooks.append(relative)

print(json.dumps({"removed_packages": sorted(found),
                  "deleted_info": deleted_info,
                  "deleted_hooks": deleted_hooks,
                  "retained_shared_packages": ["linux-base"]}, indent=2))
