#!/bin/bash
# Run as root on the installed Armbian system.
set -Eeuo pipefail
package=/var/cache/apt/archives/huastation_0.1.2_arm64.deb
curl -fL https://git.huggy.moe/qua/huastation/releases/download/0.1.2/huastation_0.1.2_arm64.deb \
    -o "$package"
printf '%s  %s\n' \
    3332efacca866c86e12bd3a40283e1cfaa01f7b4573bc3e2194e1bd39b57b2dc \
    "$package" | sha256sum -c -
apt-get update
apt-get install -y "$package"
