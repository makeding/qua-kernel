#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
	printf '%s\n' "usage: $0 /path/to/linux-7.3.4" >&2
	exit 64
fi

tree=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -d "$tree/.git" ]; then
	printf '%s\n' "target must be a git-initialized pristine Linux 7.3.4 tree: $tree" >&2
	exit 65
fi

for patch in $(cat "$root/series"); do
	git -C "$tree" apply --check "$root/patches/$patch"
	git -C "$tree" apply "$root/patches/$patch"
done

printf '%s\n' "applied Qua Station 7.3.4 series to $tree"
