#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output=${1:-out/nyampd-arm64}
runtime=${2:-}
link_flags=-static
if [[ -n "$runtime" ]]; then
  runtime=$(readlink -f "$runtime")
  link_flags=
fi

cmake -S "$source_dir" -B "$output" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER="${CC:-aarch64-linux-gnu-gcc}" \
  -DCMAKE_CXX_COMPILER="${CXX:-aarch64-linux-gnu-g++}" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DNYAMP_RKNN_ROOT="$runtime" \
  -DCMAKE_EXE_LINKER_FLAGS="$link_flags"
cmake --build "$output" -j"${JOBS:-4}"

description=$(file -Lb "$output/nyampd")
if [[ "$description" != *"ARM aarch64"* ]]; then
  echo "unexpected nyampd output: $description" >&2
  exit 1
fi

if [[ -n "$runtime" ]]; then
  python3 "$source_dir/collect_runtime_libs.py" "$output/nyampd" \
    "$runtime/lib" "$output/runtime-libs" "${CC:-aarch64-linux-gnu-gcc}"
elif [[ "$description" != *"statically linked"* ]]; then
  echo "health-only build must be static: $description" >&2
  exit 1
fi

echo "created $output/nyampd"
