#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

workspace=$(realpath "$1")
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

"$script_dir/build-sim.sh" "$workspace" cmake
"$script_dir/smoke-sim.sh" "$workspace/cmake/nuttx"
"$script_dir/build-sim.sh" "$workspace" make
"$script_dir/smoke-sim.sh" "$workspace/nuttx/nuttx"
printf 'DELIVERY_VERIFY_PASS workspace=%s\n' "$workspace"
