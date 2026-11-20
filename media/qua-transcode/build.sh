#!/bin/bash
# Build on Qua or another arm64 Linux host with the FFmpeg 4.4.5 source archive.
set -Eeuo pipefail
if [[ $# != 2 ]]; then
    echo "usage: $0 /path/to/ffmpeg-4.4.5.tar.xz /absolute/build-output" >&2
    exit 2
fi
archive=$(realpath "$1")
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$2"
out=$(realpath "$2")
expected=f9514e0d3515aee5a271283df71636e1d1ff7274b15853bcd84e144be416ab07
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
src="$out/ffmpeg-4.4.5"
if [[ ! -e "$src" ]]; then
    tar -C "$out" -xf "$archive"
    patch -d "$src" -p1 --batch < "$root/media/qua-transcode/ffmpeg-4.4.5-omx-nv12.patch"
    printf '%s\n' "$expected" > "$src/.qua-source-sha256"
else
    [[ -f "$src/.qua-source-sha256" && "$(cat "$src/.qua-source-sha256")" == "$expected" ]] || {
        echo 'existing FFmpeg build source lacks the matching Qua provenance marker' >&2; exit 1;
    }
    patch -d "$src" -p1 --batch --dry-run --reverse < "$root/media/qua-transcode/ffmpeg-4.4.5-omx-nv12.patch"
fi
cd "$src"
./configure --disable-everything --disable-doc --disable-debug --disable-x86asm \
    --enable-network --enable-ffmpeg --enable-omx \
    --extra-cflags="-I$root/media/vendor-openmax/include" \
    --enable-encoder='h264_omx,aac,rawvideo' \
    --enable-decoder='rawvideo,mpeg2video,aac' \
    --enable-demuxer='rawvideo,mpegts,nut' --enable-muxer='h264,mpegts,null,nut' \
    --enable-protocol='file,pipe,http,tcp' --enable-parser='h264,mpegvideo,aac' \
    --enable-filter='scale,yadif,fps'
make -j2
cc -O2 -Wall -Wextra -I"$root/media/vendor-openmax/include" -I"$src" \
    "$root/media/qua-transcode/transcode-direct.c" -o "$out/transcode-direct-gpubob" \
    -Wl,--start-group libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a \
    -Wl,--end-group -L"$root/media/vendor-openmax/lib" -Wl,--allow-shlib-undefined \
    -lOMX_Core -lm -pthread -ldl -latomic -lz
cc -O2 -Wall -Wextra -fPIC -shared -I/usr/include/libdrm \
    "$root/media/qua-transcode/gpu-bob.c" -o "$out/gpu-bob.so" -lEGL -lGLESv2
cc -O2 -Wall -Wextra -fPIC -shared "$root/media/qua-transcode/rpc-probe.c" -o "$out/rpc-probe.so"
cp ffmpeg "$out/ffmpeg-omx"
echo "Built in $out; install the runtime using install.sh."
