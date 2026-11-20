#!/bin/bash
# Ten seconds of Mirakurun via USB ADB; the SSH forward carries HTTP only.
set -Eeuo pipefail
qua_duration=${QUA_DURATION:-10}
if [[ ! "$qua_duration" =~ ^[0-9]+$ ]] || (( qua_duration < 1 || qua_duration > 3600 )); then
    echo 'QUA_DURATION must be 1..3600 seconds' >&2
    exit 2
fi
ssh -i "$HOME/.ssh/temp_ed25519" -o IdentitiesOnly=yes -o BatchMode=yes \
    -o ExitOnForwardFailure=yes -N -L 127.0.0.1:44072:192.168.7.254:40772 jp-noa &
qua_tunnel_pid=$!
cleanup() {
    adb -s QuaStation reverse --remove tcp:44072 >/dev/null 2>&1 || true
    kill "$qua_tunnel_pid" 2>/dev/null || true
    wait "$qua_tunnel_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for i in {1..40}; do
    if nc -z 127.0.0.1 44072; then break; fi
    kill -0 "$qua_tunnel_pid"
    sleep 0.1
done
timeout 15 adb -s QuaStation wait-for-device
adb -s QuaStation reverse tcp:44072 tcp:44072
adb -s QuaStation shell "QUA_DURATION=$qua_duration timeout -k 3 $((qua_duration + 35)) \
    /opt/qua-video/openmax/transcode-mirakurun-gpu.sh \
    http://127.0.0.1:44072/api/services/3211240960/stream /root/mirakurun-test.ts"
