#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the KICKPI-K7 N-Boot + NuttX A/B release image.

set -Eeuo pipefail

trap 'status=$?; echo "ERROR: build_nboot_ab.sh:${LINENO}: ${BASH_COMMAND}" >&2; exit "$status"' ERR

die()
{
  echo "ERROR: $*" >&2
  exit 1
}

abspath()
{
  (cd "$(dirname "$1")" && printf '%s/%s' "$(pwd)" "$(basename "$1")")
}

[ "$#" -eq 4 ] || die \
  "usage: build_nboot_ab.sh <nuttx.bin> <nboot_dir> <rkbin_dir> <out_dir>"
[ -r "$1" ] || die "NuttX image is not readable: $1"
[ -d "$2" ] || die "N-Boot release directory is missing: $2"
[ -d "$3" ] || die "rkbin directory is missing: $3"

NUTTX=$(abspath "$1")
NBOOT_DIR=$(cd "$2" && pwd)
RKBIN=$(cd "$3" && pwd)
mkdir -p "$4"
OUT=$(cd "$4" && pwd)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BOOTCTRL="$SCRIPT_DIR/../k7_abpack/bootctrl.py"
NBOOT_PROPER="$NBOOT_DIR/nboot-kickpi-k7.bin"
NBOOT_DTB="$NBOOT_DIR/nboot-kickpi-k7.dtb"
MKIMAGE="$RKBIN/tools/mkimage"
BL31="$RKBIN/bin/rk35/rk3576_bl31_v1.24.elf"
BL32="$RKBIN/bin/rk35/rk3576_bl32_v1.08.bin"

for tool in dd dtc mkfs.fat python3 sgdisk sha256sum truncate; do
  command -v "$tool" >/dev/null 2>&1 || die "missing host tool: $tool"
done
for file in "$NBOOT_PROPER" "$NBOOT_DTB" "$BOOTCTRL" "$MKIMAGE" \
            "$RKBIN/tools/boot_merger" "$RKBIN/tools/trust_merger" \
            "$RKBIN/RKBOOT/RK3576MINIALL.ini" \
            "$RKBIN/RKTRUST/RK3576TRUST.ini" "$BL31" "$BL32"; do
  [ -r "$file" ] || die "required input is not readable: $file"
done

[ "$(stat -c %s "$NUTTX")" -le $((64 * 1024 * 1024)) ] ||
  die "NuttX image exceeds its 64 MiB slot"
[ "$(stat -c %s "$NBOOT_PROPER")" -le $((2 * 1024 * 1024)) ] ||
  die "N-Boot proper exceeds 2 MiB"
[ "$(od -An -N4 -tx1 "$NBOOT_DTB" | tr -d ' \n')" = "d00dfeed" ] ||
  die "N-Boot DTB has invalid magic"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

dd if="$BL31" of="$WORK/atf-1.bin" bs=1 skip=$((0x20000)) \
  count=$((0x1e020)) status=none
dd if="$BL31" of="$WORK/atf-2.bin" bs=1 skip=$((0x40000)) \
  count=$((0x5000)) status=none
dd if="$BL31" of="$WORK/atf-3.bin" bs=1 skip=$((0x10000)) \
  count=$((0x4000)) status=none
cp "$BL32" "$WORK/optee.bin"
cp "$NBOOT_DTB" "$WORK/kickpi-k7.dtb"
cat "$NBOOT_PROPER" "$NBOOT_DTB" > "$WORK/u-boot.bin"

cat > "$WORK/nboot.its" <<'ITS'
/dts-v1/;
/ {
  description = "Nyabula N-Boot for KICKPI-K7";
  #address-cells = <1>;
  images {
    atf-1 { data = /incbin/("atf-1.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x40060000>; entry = <0x40060000>;
      hash { algo = "sha256"; }; };
    atf-2 { data = /incbin/("atf-2.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x400f0000>; hash { algo = "sha256"; }; };
    atf-3 { data = /incbin/("atf-3.bin"); type = "firmware";
      arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
      load = <0x3fe70000>; hash { algo = "sha256"; }; };
    optee { data = /incbin/("optee.bin"); type = "firmware";
      arch = "arm64"; os = "op-tee"; compression = "none";
      load = <0x48400000>; entry = <0x48400000>;
      hash { algo = "sha256"; }; };
    fdt { data = /incbin/("kickpi-k7.dtb"); type = "flat_dt";
      arch = "arm64"; compression = "none";
      hash { algo = "sha256"; }; };
    uboot { data = /incbin/("u-boot.bin"); type = "standalone";
      arch = "arm64"; compression = "none";
      load = <0x40200000>; entry = <0x40200000>;
      hash { algo = "sha256"; }; };
  };
  configurations {
    default = "conf";
    conf { firmware = "atf-1";
      loadables = "atf-2", "atf-3", "optee", "uboot"; fdt = "fdt"; };
  };
};
ITS

(cd "$WORK" && "$MKIMAGE" -E -p 0x1000 -f nboot.its nboot.fit >/dev/null)
# The pinned rkbin mkimage leaks its mmap address into the FIT reservation
# entry. N-Boot does not consume that reservation, so terminate the map at its
# first entry to make release output reproducible.
dd if=/dev/zero of="$WORK/nboot.fit" bs=1 seek=40 count=16 \
  conv=notrunc status=none
[ "$(stat -c %s "$WORK/nboot.fit")" -le $((4 * 1024 * 1024)) ] ||
  die "N-Boot FIT exceeds its 4 MiB region"
cp "$WORK/nboot.fit" "$OUT/nboot.img"
truncate -s $((4 * 1024 * 1024)) "$OUT/nboot.img"

RKBIN_WORK="$WORK/rkbin"
cp -r "$RKBIN" "$RKBIN_WORK"
chmod -R u+w "$RKBIN_WORK"
(cd "$RKBIN_WORK" && ./tools/boot_merger RKBOOT/RK3576MINIALL.ini >/dev/null)
(cd "$RKBIN_WORK" && ./tools/trust_merger RKTRUST/RK3576TRUST.ini >/dev/null)
cp "$RKBIN_WORK"/rk3576_idblock_*.img "$WORK/idbloader.img"
cp "$RKBIN_WORK/trust.img" "$WORK/trust.img"

python3 "$BOOTCTRL" init --output "$WORK/bootctrl.bin" \
  --nuttx-a "$NUTTX" --nuttx-b "$NUTTX"
python3 "$BOOTCTRL" inspect "$WORK/bootctrl.bin" >/dev/null

IMAGE="$OUT/nyabula-k7-sd.img"
truncate -s 4G "$IMAGE"
DATA_START=2396160
DATA_END=$((4 * 1024 * 1024 * 1024 / 512 - 2049))
sgdisk -og "$IMAGE" >/dev/null
sgdisk -U 4b374142-0000-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 1:16384:24575 -c 1:uboot -t 1:8300 \
  -u 1:4b374142-0001-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 2:24576:32767 -c 2:trust -t 2:8300 \
  -u 2:4b374142-0002-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 3:32768:34815 -c 3:bootctrl -t 3:8300 \
  -u 3:4b374142-0003-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 4:36864:167935 -c 4:nuttx_a -t 4:8300 \
  -u 4:4b374142-0004-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 5:167936:299007 -c 5:nuttx_b -t 5:8300 \
  -u 5:4b374142-0005-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 6:299008:1347583 -c 6:amp_a -t 6:8300 \
  -u 6:4b374142-0006-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 7:1347584:2396159 -c 7:amp_b -t 7:8300 \
  -u 7:4b374142-0007-4000-8000-000000000002 "$IMAGE" >/dev/null
sgdisk -n 8:"$DATA_START":"$DATA_END" -c 8:data -t 8:0700 \
  -u 8:4b374142-0008-4000-8000-000000000002 "$IMAGE" >/dev/null

dd if="$WORK/idbloader.img" of="$IMAGE" bs=512 seek=64 \
  conv=notrunc status=none
if [ "$(stat -c %s "$WORK/idbloader.img")" -le $((1024 * 512)) ]; then
  dd if="$WORK/idbloader.img" of="$IMAGE" bs=512 seek=1088 \
    conv=notrunc status=none
fi
dd if="$OUT/nboot.img" of="$IMAGE" bs=512 seek=16384 \
  conv=notrunc status=none
dd if="$WORK/trust.img" of="$IMAGE" bs=512 seek=24576 \
  conv=notrunc status=none
dd if="$WORK/bootctrl.bin" of="$IMAGE" bs=512 seek=32768 \
  conv=notrunc status=none
dd if="$NUTTX" of="$IMAGE" bs=512 seek=36864 conv=notrunc status=none
dd if="$NUTTX" of="$IMAGE" bs=512 seek=167936 conv=notrunc status=none

DATA_SECTORS=$((DATA_END - DATA_START + 1))
truncate -s $((DATA_SECTORS * 512)) "$WORK/data.fat"
mkfs.fat --invariant -F 32 -S 512 -n NYABULA "$WORK/data.fat" >/dev/null
dd if="$WORK/data.fat" of="$IMAGE" bs=512 seek="$DATA_START" \
  conv=notrunc,sparse status=none
sgdisk -v "$IMAGE"

sha256sum "$NUTTX" "$OUT/nboot.img" "$IMAGE" > "$OUT/SHA256SUMS"
printf 'OK: %s\n' "$IMAGE"
