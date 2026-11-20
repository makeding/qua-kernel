#!/bin/bash
set -Eeuo pipefail
if [[ $# != 1 ]]; then echo "usage: $0 /absolute/build-output" >&2; exit 2; fi
out=$(realpath "$1")
root=$(cd "$(dirname "$0")/../.." && pwd)
dest=/opt/qua-video/openmax
for name in transcode-direct-gpubob gpu-bob.so rpc-probe.so ffmpeg-omx; do test -f "$out/$name"; done
install -d "$dest/lib"
cp -a "$root/media/vendor-openmax/lib/." "$dest/lib/"
install -m 755 "$out/transcode-direct-gpubob" "$out/gpu-bob.so" "$out/rpc-probe.so" "$out/ffmpeg-omx" "$dest/"
install -m 755 "$root/media/qua-transcode/transcode-mirakurun-gpu.sh" "$dest/"
echo "Installed Qua media runtime in $dest"
