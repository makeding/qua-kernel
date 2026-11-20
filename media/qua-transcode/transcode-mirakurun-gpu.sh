#!/bin/bash
# QuaStation MPEG-2 1080i -> H.264 progressive, 29.97 fps, GPU field interpolation.
set -Eeuo pipefail
if [ "$#" -ne 2 ]; then
    printf 'usage: %s input-url output.ts\n' "$0" >&2
    exit 2
fi
exec 9>/run/lock/qua-ve-encode.lock
flock -n 9
trap 'rm -f /dev/shm/sem.omx0 /dev/shm/sem.VPU1 /dev/shm/sem.VPU2' EXIT
python3 - <<'PY'
import fcntl, os, struct
fd = os.open('/dev/vpu', os.O_RDWR)
try:
    fcntl.ioctl(fd, 0x5610, struct.pack('III', 0, 1, 0))
    fcntl.ioctl(fd, 0x5604, struct.pack('I', 0))
    fcntl.ioctl(fd, 0x5610, struct.pack('III', 0, 0, 0))
finally:
    os.close(fd)
PY
# The 1440x1080 NHK stream is 16:9, with SAR 4:3 and BT.709 limited range.
env LD_LIBRARY_PATH=/opt/qua-video/openmax/lib \
    LD_PRELOAD=/opt/qua-video/openmax/rpc-probe.so \
    QUA_DEINTERLACE="${QUA_DEINTERLACE:-bob-tff}" QUA_GPU_IO=dmabuf \
    QUA_DURATION="${QUA_DURATION:-10}" \
    /opt/qua-video/openmax/transcode-direct-gpubob "$1" pipe:1 |
    /usr/bin/ffmpeg -hide_banner -loglevel warning -f mpegts -probesize 262144 \
      -analyzeduration 500000 -i pipe:0 -map 0:v:0 -map 0:a:0 -c copy \
      -bsf:v 'h264_metadata=sample_aspect_ratio=4/3:video_full_range_flag=0:colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1' \
      -f mpegts -y "$2"
