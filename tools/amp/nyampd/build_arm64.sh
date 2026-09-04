#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output=${1:-out/nyampd-arm64}

cmake -S "$source_dir" -B "$output" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="${CC:-aarch64-linux-gnu-gcc}" \
  -DCMAKE_CXX_COMPILER="${CXX:-aarch64-linux-gnu-g++}" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build "$output" -j"${JOBS:-4}"

description=$(file -Lb "$output/nyampd")
if [[ "$description" != *"ARM aarch64"* ||
      "$description" != *"statically linked"* ]]; then
  echo "unexpected nyampd output: $description" >&2
  exit 1
fi

echo "created $output/nyampd"
