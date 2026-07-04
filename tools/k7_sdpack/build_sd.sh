#!/bin/bash
# Build a KICKPI-K7 (RK3576) SD boot image with NuttX as BL33, entirely from
# rkbin official Rockchip binaries. No device-extracted blobs.
#
#   idbloader.img (DDR + U-Boot SPL)          <- rkbin boot_merger  RK3576MINIALL.ini
#   uboot_nuttx.img (FIT: nuttx + BL31 + OP-TEE) <- authored fit.its + rkbin ATF/OP-TEE
#   trust.img (BL31 + BL32)                   <- rkbin trust_merger RK3576TRUST.ini
#   sd_nuttx.img = GPT + idbloader@64 + uboot@16384 + trust@24576
#
# Verified on board 2026-07-04: boots SD -> SPL(FIT all segments OK, no bad hash)
# -> BL31 v1.24 -> OP-TEE v1.08 -> EL3 exit@EL2 -> NuttShell (NSH).
#
# Usage: ./build_sd.sh <nuttx.bin> <rkbin_dir> <out_dir>
#   Get <rkbin_dir> with ./fetch_rkbin.sh (or point at a full rkbin checkout).
set -e
abspath() { (cd "$(dirname "$1")" && printf '%s/%s' "$(pwd)" "$(basename "$1")"); }
NUTTX=$(abspath "${1:?usage: build_sd.sh <nuttx.bin> <rkbin_dir> <out_dir>}")
RKBIN=$(cd "${2:?rkbin dir}" && pwd)
OUT="${3:?out dir}"; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
MKIMAGE=$RKBIN/tools/mkimage

# Pinned rkbin versions (match RK3576MINIALL.ini / RK3576TRUST.ini).
BL31=$RKBIN/bin/rk35/rk3576_bl31_v1.24.elf
BL32=$RKBIN/bin/rk35/rk3576_bl32_v1.08.bin

cd "$OUT"

# --- 1. split BL31 elf PT_LOAD segments -> atf-N.bin ---------------------------
# SoC-fixed load addresses (verified identical to the vendor FIT atf-1/2/3):
#   0x40060000 entry/firmware (atf-1) | 0x400f0000 (atf-2) | 0x3fe70000 (atf-3)
# Offsets/sizes below are the elf's PT_LOAD file offset/FileSiz (readelf -l).
dd if="$BL31" of=atf-1.bin bs=1 skip=$((0x20000)) count=$((0x1e020)) status=none
dd if="$BL31" of=atf-2.bin bs=1 skip=$((0x40000)) count=$((0x5000))  status=none
dd if="$BL31" of=atf-3.bin bs=1 skip=$((0x10000)) count=$((0x4000))  status=none
cp "$BL32" optee.bin
cp "$NUTTX" nuttx.bin

# --- 2. dummy control fdt (nuttx does not use it; the FIT config wants one) ----
echo '/dts-v1/; / { compatible = "rockchip,rk3576"; };' | dtc -O dtb -o dummy.dtb 2>/dev/null

# --- 3. author FIT and pack ----------------------------------------------------
# IMPORTANT: no `-E` (external data). Embedded data keeps the FIT self-consistent;
# mkimage recomputes every node hash. This is what avoids the atf-3 "Bad hash"
# that plagued the old surgical vendor-FIT patch (see README).
cat > fit.its <<'ITS'
/dts-v1/;
/ {
    description = "NuttX BL33 + rkbin ATF/OP-TEE for RK3576 (KICKPI-K7)";
    #address-cells = <1>;
    images {
        uboot { description = "NuttX (BL33)"; data = /incbin/("nuttx.bin");
            type = "standalone"; arch = "arm64"; compression = "none";
            load = <0x40200000>; entry = <0x40200000>; hash { algo = "sha256"; }; };
        atf-1 { description = "ARM Trusted Firmware"; data = /incbin/("atf-1.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x40060000>; entry = <0x40060000>; hash { algo = "sha256"; }; };
        atf-2 { description = "ARM Trusted Firmware"; data = /incbin/("atf-2.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x400f0000>; hash { algo = "sha256"; }; };
        atf-3 { description = "ARM Trusted Firmware"; data = /incbin/("atf-3.bin");
            type = "firmware"; arch = "arm64"; os = "arm-trusted-firmware"; compression = "none";
            load = <0x3fe70000>; hash { algo = "sha256"; }; };
        optee { description = "OP-TEE"; data = /incbin/("optee.bin");
            type = "firmware"; arch = "arm64"; os = "op-tee"; compression = "none";
            load = <0x48400000>; entry = <0x48400000>; hash { algo = "sha256"; }; };
        fdt { description = "dummy fdt"; data = /incbin/("dummy.dtb");
            type = "flat_dt"; arch = "arm64"; compression = "none"; hash { algo = "sha256"; }; };
    };
    configurations {
        default = "conf";
        conf { description = "rk3576 nuttx"; firmware = "atf-1";
            loadables = "uboot", "atf-2", "atf-3", "optee"; fdt = "fdt"; };
    };
};
ITS
"$MKIMAGE" -f fit.its uboot_nuttx.img >/dev/null
echo "FIT: uboot_nuttx.img $(stat -c%s uboot_nuttx.img) bytes"

# --- 4. idbloader + trust from rkbin (mergers read ini paths relative to root) -
( cd "$RKBIN" && ./tools/boot_merger  RKBOOT/RK3576MINIALL.ini )
( cd "$RKBIN" && ./tools/trust_merger RKTRUST/RK3576TRUST.ini )
cp "$RKBIN"/rk3576_idblock_*.img idbloader.img
cp "$RKBIN"/trust.img trust.img

# --- 5. assemble SD image (Rockchip standard sector offsets) -------------------
IMG=sd_nuttx.img
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
sgdisk -og "$IMG" >/dev/null 2>&1
# Fixed disk/partition GUIDs -> stable GPT (the FIT header still carries a build
# timestamp, so the whole image is not bit-reproducible; the payloads are).
sgdisk -U 4b494b50-4937-4b37-b364-72616d626f6f "$IMG" >/dev/null 2>&1
sgdisk -n 1:16384:24575 -c 1:uboot  -t 1:8300 -u 1:4b494b50-0001-4b37-b364-72616d626f6f "$IMG" >/dev/null
sgdisk -n 2:24576:32767 -c 2:trust  -t 2:8300 -u 2:4b494b50-0002-4b37-b364-72616d626f6f "$IMG" >/dev/null
sgdisk -n 3:32768:0     -c 3:rootfs -t 3:8300 -u 3:4b494b50-0003-4b37-b364-72616d626f6f "$IMG" >/dev/null
dd if=idbloader.img   of="$IMG" bs=512 seek=64    conv=notrunc status=none
dd if=uboot_nuttx.img of="$IMG" bs=512 seek=16384 conv=notrunc status=none
dd if=trust.img       of="$IMG" bs=512 seek=24576 conv=notrunc status=none
echo "OK -> $OUT/$IMG ($(stat -c%s "$IMG") bytes)"
