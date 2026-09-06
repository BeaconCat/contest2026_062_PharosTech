#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

workspace=$(realpath "$1")
backend=${2:-all}
jobs=${JOBS:-4}
tools=${OPENVELA_TOOLS:-/root/openvela/src/vela/prebuilts/tools}

test -f "$workspace/nuttx/boards/sim/sim/configs/nyabula_core/defconfig"
export PATH="$tools/python/bin:$tools/cmake/bin:$PATH"
export PYTHONPATH="$tools/python/dist-packages/kconfiglib"

build_make()
{
  cd "$workspace/nuttx"
  ./tools/configure.sh -l sim:nyabula_core
  make -j"$jobs"
  printf 'DELIVERY_MAKE_PASS binary=%s/nuttx/nuttx\n' "$workspace"
}

build_cmake()
{
  cmake -S "$workspace/nuttx" -B "$workspace/cmake" -GNinja \
    -DBOARD_CONFIG=sim:nyabula_core \
    -DKCONFIGLIB="$tools/python/bin/olddefconfig"
  cmake --build "$workspace/cmake" -j"$jobs"
  printf 'DELIVERY_CMAKE_PASS binary=%s/cmake/nuttx\n' "$workspace"
}

case "$backend" in
  make) build_make ;;
  cmake) build_cmake ;;
  all)
    build_cmake
    build_make
    ;;
  *)
    echo 'Backend must be make, cmake, or all' >&2
    exit 2
    ;;
esac
