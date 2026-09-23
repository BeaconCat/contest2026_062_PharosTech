#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
workspace=$(realpath "$1")
jobs=${JOBS:-4}
test -f "$workspace/nuttx/boards/arm64/qemu/qemu-armv8a/configs/nyabula_delivery/defconfig"
export PATH=/root/openvela/src/vela/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
export PYTHONPATH=/root/openvela/src/vela/prebuilts/tools/python/dist-packages/kconfiglib
cd "$workspace/nuttx"
./tools/configure.sh -l qemu-armv8a:nyabula_delivery
make -j"$jobs"
make export -j1
printf 'KERNEL_READY=%s/nuttx/nuttx\n' "$workspace"
