#!/bin/bash
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

[ "$#" -eq 4 ] || {
  echo "usage: build_emmc.sh <nuttx.bin> <nboot_dir> <rkbin_dir> <out_dir>" >&2
  exit 1
}

exec bash "$SCRIPT_DIR/build_nboot_ab.sh" "$1" "$2" "$3" "$4" emmc
