#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# != 1 ]]; then
  echo "usage: $0 NUTTX_SOURCE_DIRECTORY" >&2
  exit 2
fi

source_dir=$(realpath "$1")
patch_file=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/gicv2-amp.patch

if git -C "$source_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
  echo "NuttX GICv2 AMP patch is already applied"
else
  git -C "$source_dir" apply --check "$patch_file"
  git -C "$source_dir" apply "$patch_file"
  echo "Applied NuttX GICv2 AMP patch to $source_dir"
fi
