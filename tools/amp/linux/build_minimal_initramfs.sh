#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail
umask 022

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: $0 BUSYBOX [OUTPUT.cpio.gz] [STATIC_NYAMPD]" >&2
  exit 2
fi

busybox=$(readlink -f "$1")
output=${2:-nyabula-amp-initramfs.cpio.gz}
nyampd=${3:-}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ ! -x "$busybox" ]]; then
  echo "busybox is not executable: $busybox" >&2
  exit 2
fi

description=$(file -Lb "$busybox")
if [[ "$description" != *"ARM aarch64"* ||
      "$description" != *"statically linked"* ]]; then
  echo "busybox must be a statically linked AArch64 ELF: $description" >&2
  exit 2
fi

if [[ -n "$nyampd" ]]; then
  nyampd=$(readlink -f "$nyampd")
  description=$(file -Lb "$nyampd")
  if [[ ! -x "$nyampd" || "$description" != *"ARM aarch64"* ||
        "$description" != *"statically linked"* ]]; then
    echo "nyampd must be a statically linked AArch64 ELF: $description" >&2
    exit 2
  fi
fi

if ! command -v cpio >/dev/null; then
  echo "cpio is required" >&2
  exit 2
fi

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
epoch=${SOURCE_DATE_EPOCH:-0}

mkdir -p "$work"/{bin,sbin,etc,proc,sys,dev,run,tmp,usr/bin,usr/sbin}
cp "$busybox" "$work/bin/busybox"
cp "$script_dir/init" "$work/init"
chmod 0755 "$work/init" "$work/bin/busybox"

for applet in sh mount mkdir echo; do
  ln -s busybox "$work/bin/$applet"
done
ln -s ../bin/busybox "$work/sbin/mdev"
ln -s ../bin/busybox "$work/sbin/init"
: > "$work/etc/inittab"

if [[ -n "$nyampd" ]]; then
  cp "$nyampd" "$work/usr/sbin/nyampd"
  chmod 0755 "$work/usr/sbin/nyampd"
  cp "$script_dir/inittab" "$work/etc/inittab"
fi

find "$work" -exec touch -h -d "@$epoch" {} +

(
  cd "$work"
  find . -print0 | LC_ALL=C sort -z | \
    cpio --null -o -H newc --quiet --reproducible --owner=0:0
) | gzip -n -9 > "$output"

echo "created $output ($(stat -c%s "$output") bytes)"
