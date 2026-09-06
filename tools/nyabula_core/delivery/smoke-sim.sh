#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

binary=$(realpath "$1")
output=$(mktemp)
trap 'rm -f "$output"' EXIT

printf 'nycore isolation\nnycore list\nhello\npoweroff\n' | \
  timeout 10 "$binary" >"$output" 2>&1
grep -q 'build=flat current_el=host' "$output"
grep -q 'Hello, World!!' "$output"
if grep -q 'nycore: command failed' "$output"; then
  cat "$output"
  exit 1
fi

printf 'DELIVERY_SMOKE_PASS binary=%s\n' "$binary"
