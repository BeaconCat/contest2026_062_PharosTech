#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

output=${1:-busybox-arm64}
url=https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6+b8_arm64.deb
expected=6d144e5012d47ec3a6f2102ba6fac644ad34c989373fed30c1bb7264f5cd3616
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

if command -v curl >/dev/null; then
  curl --fail --location --retry 3 --output "$work/busybox.deb" "$url"
elif command -v wget >/dev/null; then
  wget --tries=3 --output-document="$work/busybox.deb" "$url"
else
  echo "curl or wget is required" >&2
  exit 2
fi

actual=$(sha256sum "$work/busybox.deb" | awk '{print $1}')
if [[ "$actual" != "$expected" ]]; then
  echo "busybox package checksum mismatch: $actual" >&2
  exit 1
fi

dpkg-deb -x "$work/busybox.deb" "$work/root"
install -m 0755 "$work/root/usr/bin/busybox" "$output"
echo "installed $output"
