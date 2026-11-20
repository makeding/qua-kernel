#!/bin/bash
set -Eeuo pipefail
cd "$(dirname "$0")"
mkdir -p lib
base=https://raw.githubusercontent.com/tsukumijima/QuaStation-Kernel/041115133e6babea21a0e75519648afeb023b7ac/prebuilt/openmax/usr/lib
while read -r hash path; do
    [[ "$path" == lib/* ]] || continue
    curl -fL "$base/${path#lib/}" -o "$path"
done < SHA256SUMS
sha256sum -c SHA256SUMS
