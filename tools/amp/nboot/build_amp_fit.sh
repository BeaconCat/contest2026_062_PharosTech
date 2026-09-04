#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  echo "usage: $0 LINUX_IMAGE K7_DTB INITRAMFS OPENVELA_BIN [OUTPUT.itb]" >&2
  exit 2
fi

kernel=$(readlink -f "$1")
fdt=$(readlink -f "$2")
ramdisk=$(readlink -f "$3")
openvela=$(readlink -f "$4")
output=${5:-nyabula-amp.itb}

for input in "$kernel" "$fdt" "$ramdisk" "$openvela"; do
  if [[ ! -f "$input" ]]; then
    echo "missing input: $input" >&2
    exit 2
  fi
done

for tool in mkimage fdtget gzip; do
  if ! command -v "$tool" >/dev/null; then
    echo "missing tool: $tool" >&2
    exit 2
  fi
done

if [[ $(od -An -tx1 -j56 -N4 "$kernel" | tr -d ' \n') != 41524d64 ]]; then
  echo "Linux Image header magic is invalid" >&2
  exit 1
fi

if [[ $(od -An -tx1 -N4 "$fdt" | tr -d ' \n') != d00dfeed ]]; then
  echo "K7 DTB magic is invalid" >&2
  exit 1
fi

gzip -t "$ramdisk"

check_size()
{
  local path=$1
  local limit=$2
  local label=$3

  if (( $(stat -c%s "$path") > limit )); then
    echo "$label exceeds its load window" >&2
    exit 1
  fi
}

check_size "$kernel" 0x05000000 "Linux Image"
check_size "$fdt" 0x01000000 "K7 DTB"
check_size "$ramdisk" 0x10000000 "initramfs"
check_size "$openvela" 0x01000000 "openvela image"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
its="$work/nyabula-amp.its"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}

{
  printf '/dts-v1/;\n\n/ {\n'
  printf '  description = "Nyabula KICKPI-K7 AMP";\n'
  printf '  #address-cells = <2>;\n\n'
  printf '  images {\n'
  printf '    linux {\n'
  printf '      description = "RK3576 Linux compute domain";\n'
  printf '      data = /incbin/("%s");\n' "$kernel"
  printf '      type = "kernel"; arch = "arm64"; os = "linux";\n'
  printf '      compression = "none";\n'
  printf '      load = <0x0 0x42000000>; entry = <0x0 0x42000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    fdt {\n'
  printf '      description = "KICKPI-K7 AMP device tree";\n'
  printf '      data = /incbin/("%s");\n' "$fdt"
  printf '      type = "flat_dt"; arch = "arm64"; compression = "none";\n'
  printf '      load = <0x0 0x4f000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    ramdisk {\n'
  printf '      description = "Nyabula AMP initramfs";\n'
  printf '      data = /incbin/("%s");\n' "$ramdisk"
  printf '      type = "ramdisk"; arch = "arm64"; os = "linux";\n'
  printf '      compression = "gzip"; load = <0x0 0x50000000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '    openvela {\n'
  printf '      description = "openvela CPU3 control domain";\n'
  printf '      data = /incbin/("%s");\n' "$openvela"
  printf '      type = "firmware"; arch = "arm64"; compression = "none";\n'
  printf '      cpu = <0x3>;\n'
  printf '      load = <0x0 0x4a400000>; entry = <0x0 0x4a400000>;\n'
  printf '      hash { algo = "sha256"; };\n'
  printf '    };\n'
  printf '  };\n\n'
  printf '  configurations {\n'
  printf '    default = "conf";\n'
  printf '    conf {\n'
  printf '      kernel = "linux"; fdt = "fdt"; ramdisk = "ramdisk";\n'
  printf '      loadables = "openvela";\n'
  printf '    };\n'
  printf '  };\n'
  printf '};\n'
} > "$its"

mkimage -E -p 0x1000 -f "$its" "$output" >/dev/null

[[ $(fdtget -t x "$output" /images/linux load) == "0 42000000" ]]
[[ $(fdtget -t x "$output" /images/openvela load) == "0 4a400000" ]]
[[ $(fdtget -t x "$output" /images/openvela entry) == "0 4a400000" ]]
[[ $(fdtget -t x "$output" /images/openvela cpu) == "3" ]]

echo "created $output ($(stat -c%s "$output") bytes)"
sha256sum "$output"
