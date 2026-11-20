#!/bin/bash
# Run as root on the installed Armbian system.
set -Eeuo pipefail
package=/var/cache/apt/archives/huastation_0.1.0_arm64.deb
curl -fL https://git.huggy.moe/qua/huastation/releases/download/0.1.0/huastation_0.1.0_arm64.deb \
    -o "$package"
printf '%s  %s\n' \
    bd10cc1f6a7514e5dfdedb53cb0a6f4a3426dc6a45b8e8271683c3b8e53c7072 \
    "$package" | sha256sum -c -
apt-get update
apt-get install -y "$package"
