#!/bin/bash
# Finish Debian arm64 package configuration on an x86-64 build host.
set -Eeuo pipefail
root=${1:?usage: finish-cross-rootfs.sh ROOTFS}
root=$(realpath "$root")
[[ "$root" != / && -d "$root/var/lib/dpkg" ]]
qemu=/usr/bin/qemu-aarch64-static
[[ -x "$qemu" ]]
for p in "$root/usr/bin/fc-cache.qua-original" "$root/usr/bin/qemu-aarch64-static" \
         "$root/usr/sbin/policy-rc.d" "$root/run/qua-fc-cache.sock"; do
    [[ ! -e "$p" ]] || { echo "temporary path already exists: $p" >&2; exit 1; }
done
server=
cleanup() {
    rc=$?
    trap - EXIT INT TERM
    if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
    if [[ -e "$root/usr/bin/fc-cache.qua-original" ]]; then
        mv -f "$root/usr/bin/fc-cache.qua-original" "$root/usr/bin/fc-cache"
    fi
    for p in "$root/usr/bin/qemu-aarch64-static" "$root/usr/sbin/policy-rc.d" "$root/run/qua-fc-cache.sock"; do
        [[ ! -e "$p" ]] || rm -- "$p"
    done
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp "$qemu" "$root/usr/bin/qemu-aarch64-static"
mv "$root/usr/bin/fc-cache" "$root/usr/bin/fc-cache.qua-original"
cat > "$root/usr/bin/fc-cache" <<'PY'
#!/usr/bin/python3
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/run/qua-fc-cache.sock')
s.sendall(json.dumps(sys.argv[1:]).encode() + b'\n')
reply = b''
while not reply.endswith(b'\n'):
    part = s.recv(128)
    if not part:
        raise RuntimeError('font cache bridge disconnected')
    reply += part
sys.exit(int(reply))
PY
chmod 755 "$root/usr/bin/fc-cache"
printf '#!/bin/sh\nexit 101\n' > "$root/usr/sbin/policy-rc.d"
chmod 755 "$root/usr/sbin/policy-rc.d"
python3 - "$root" <<'PY' &
import json, os, socket, subprocess, sys
root = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(root + '/run/qua-fc-cache.sock')
sock.listen(1)
while True:
    connection, _ = sock.accept()
    with connection:
        request = b''
        while not request.endswith(b'\n') and len(request) < 4096:
            part = connection.recv(4096)
            if not part:
                break
            request += part
        args = json.loads(request)
        result = subprocess.run(['chroot', root, '/usr/bin/qemu-aarch64-static',
                                 '/usr/bin/fc-cache.qua-original', *args], check=False)
        connection.sendall((str(result.returncode) + '\n').encode())
PY
server=$!
for _ in $(seq 1 100); do [[ -S "$root/run/qua-fc-cache.sock" ]] && break; kill -0 "$server"; sleep .05; done
[[ -S "$root/run/qua-fc-cache.sock" ]]
proot -0 -q "$qemu" -r "$root" -b /dev -b /proc -b /sys -w / \
    /usr/bin/env DEBIAN_FRONTEND=noninteractive /usr/bin/dpkg --configure -a
audit=$(proot -0 -q "$qemu" -r "$root" -b /dev -b /proc -b /sys -w / /usr/bin/dpkg --audit)
[[ -z "$audit" ]] || { printf '%s\n' "$audit" >&2; exit 1; }
echo PUBLIC_ROOTFS_DPKG_CONFIGURED
