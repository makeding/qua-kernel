#!/bin/bash
# Run as root on the installed Armbian system.
set -Eeuo pipefail
package=/var/cache/apt/archives/huastation_0.1.1_arm64.deb
curl -fL https://git.huggy.moe/qua/huastation/releases/download/0.1.1/huastation_0.1.1_arm64.deb \
    -o "$package"
printf '%s  %s\n' \
    43dcd6e6fcb8d6d0ea22e6a41284a5677329c3c75677d118a4b339e49f4966c9 \
    "$package" | sha256sum -c -
apt-get update
apt-get install -y "$package"
