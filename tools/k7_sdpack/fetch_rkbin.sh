#!/bin/bash
# Fetch exactly the RK3576 files needed by build_sd.sh from the official
# Rockchip binary repo (github.com/rockchip-linux/rkbin). Downloads ~4MB of
# specific files instead of cloning the full ~1.5GB repo.
#
# Usage: ./fetch_rkbin.sh [out_rkbin_dir]
#   Honors http_proxy/https_proxy env (or set PROXY=host:port).
#   Alternatively, skip this and point build_sd.sh at a full rkbin checkout.
set -e
DEST="${1:-rkbin}"
REV="master"   # rkbin has no release tags; pin a commit here for full reproducibility
BASE="https://raw.githubusercontent.com/rockchip-linux/rkbin/$REV"
CURL="curl -sSL -m 180"
[ -n "$PROXY" ] && CURL="$CURL -x http://$PROXY"

FILES="
RKBOOT/RK3576MINIALL.ini
RKTRUST/RK3576TRUST.ini
bin/rk35/rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.12.bin
bin/rk35/rk3576_spl_v1.08.bin
bin/rk35/rk3576_bl31_v1.24.elf
bin/rk35/rk3576_bl32_v1.08.bin
bin/rk35/rk3576_boost_v1.03.bin
bin/rk35/rk3576_usbplug_v1.04.bin
tools/boot_merger
tools/trust_merger
tools/mkimage
tools/loaderimage
"
for f in $FILES; do
    mkdir -p "$DEST/$(dirname "$f")"
    $CURL "$BASE/$f" -o "$DEST/$f"
    echo "  $(stat -c%s "$DEST/$f" 2>/dev/null || echo '??') $f"
done
chmod +x "$DEST"/tools/* 2>/dev/null || true
echo "rkbin files -> $DEST"
