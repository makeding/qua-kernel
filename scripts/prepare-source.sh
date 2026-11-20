#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$1
archive="$build/linux-7.3.4.tar.xz"
tree="$build/linux-7.3.4"
mkdir -p "$build"
if [ ! -f "$archive" ]; then
  curl -fL -o "$archive" https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.3.4.tar.xz
fi
printf '%s  %s\n' "$(cat "$root/UPSTREAM-SHA256")" "$archive" | sha256sum -c -
if [ ! -d "$tree" ]; then
  tar -xJf "$archive" -C "$build"
  git -C "$tree" init -q
  "$root/scripts/apply-series.sh" "$tree"
fi
