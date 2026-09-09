#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 KERNEL_SOURCE OUTPUT_DIR [BASE_DTS]" >&2
  exit 2
fi

kernel=$(readlink -f "$1")
output=$(mkdir -p "$2" && readlink -f "$2")
base_dts=${3:-rk3576-kickpi-k7-android.dts}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
dts_dir="$kernel/arch/arm64/boot/dts/rockchip"
generated_dts="$dts_dir/rk3576-kickpi-k7-nyabula-amp.dts"
installed_dtsi="$dts_dir/rk3576-kickpi-k7-nyabula-amp.dtsi"

if [[ ! -f "$kernel/Makefile" || ! -f "$dts_dir/$base_dts" ]]; then
  echo "invalid kernel source or missing base DTS: $dts_dir/$base_dts" >&2
  exit 2
fi

cross_compile=${CROSS_COMPILE:-aarch64-linux-gnu-}
if ! command -v "${cross_compile}gcc" >/dev/null; then
  echo "missing cross compiler: ${cross_compile}gcc" >&2
  exit 2
fi

if [[ -e "$generated_dts" || -L "$generated_dts" ||
      -e "$installed_dtsi" || -L "$installed_dtsi" ]]; then
  echo "refusing to overwrite existing AMP device-tree inputs" >&2
  exit 2
fi

cleanup()
{
  rm -f -- "$generated_dts" "$installed_dtsi"
}
trap cleanup EXIT

cp "$script_dir/rk3576-kickpi-k7-amp.dtsi" "$installed_dtsi"
printf '#include "%s"\n#include "%s"\n' \
  "$base_dts" "$(basename "$installed_dtsi")" > "$generated_dts"

make -C "$kernel" O="$output" ARCH=arm64 \
  CROSS_COMPILE="$cross_compile" rockchip_linux_defconfig
fragments=()
if [[ -f "$kernel/arch/arm64/configs/rockchip_amp.config" ]]; then
  fragments+=("$kernel/arch/arm64/configs/rockchip_amp.config")
fi
fragments+=("$script_dir/nyabula_amp.fragment")
"$kernel/scripts/kconfig/merge_config.sh" -m -O "$output" \
  "$output/.config" "${fragments[@]}"
make -C "$kernel" O="$output" ARCH=arm64 \
  CROSS_COMPILE="$cross_compile" olddefconfig

for option in ROCKCHIP_AMP ROCKCHIP_MBOX RPMSG_ROCKCHIP_MBOX RPMSG_VIRTIO RPMSG_NS \
              RPMSG_CHAR RPMSG_CTRL ROCKCHIP_RKNPU BLK_DEV_INITRD NULL_TTY; do
  if ! grep -q "^CONFIG_${option}=y$" "$output/.config"; then
    echo "required option was not selected: CONFIG_$option" >&2
    exit 1
  fi
done

for option in FIQ_DEBUGGER CMDLINE_FORCE CPU_FREQ CPU_IDLE \
              ARM_ROCKCHIP_DMC_DEVFREQ SUSPEND HIBERNATION ROCKCHIP_SUSPEND_MODE \
              INPUT HID BATTERY_RK817 CHARGER_RK817 RK_HEADSET; do
  if grep -q "^CONFIG_${option}=y$" "$output/.config"; then
    echo "unsafe AMP option remained enabled: CONFIG_$option" >&2
    exit 1
  fi
done

targets=(Image rockchip/rk3576-kickpi-k7-nyabula-amp.dtb)
if grep -q '^CONFIG_MODULES=y$' "$output/.config"; then
  targets+=(modules)
fi

make -C "$kernel" O="$output" ARCH=arm64 \
  CROSS_COMPILE="$cross_compile" -j"${JOBS:-4}" "${targets[@]}"

echo "kernel: $output/arch/arm64/boot/Image"
echo "dtb: $output/arch/arm64/boot/dts/rockchip/"\
"rk3576-kickpi-k7-nyabula-amp.dtb"
