#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
src=$(realpath "$1")
team=$(realpath "$2")
dst=$(realpath -m "$3")
sqlite_cache=${4:-}
case "$dst" in /*/nyabula-delivery-*) ;; *) echo 'Output must be a dedicated nyabula-delivery-* directory' >&2; exit 1;; esac
test -d "$src/nuttx"
test -d "$src/apps"
test ! -e "$dst"
mkdir "$dst"
for part in nuttx apps packages external vendor; do
  mkdir "$dst/$part"
  rsync -a --exclude=.git --exclude='*.o' --exclude='*.a' --exclude='*.d' \
    --exclude=.built --exclude=.depend --exclude=Make.dep \
    --exclude=.config --exclude=.config.old "$src/$part/" "$dst/$part/"
done
for path in nuttx/include/arch packages/demos/contest2026_062_nyabula_core; do
  if [ -L "$dst/$path" ]; then unlink "$dst/$path"; fi
done
for path in nuttx/include/nuttx/config.h apps/builtin/builtin_list.h apps/builtin/builtin_proto.h; do
  if [ -f "$dst/$path" ]; then mv "$dst/$path" "$dst/$(basename "$path").previous"; fi
done
mkdir -p "$dst/packages/demos/contest2026_062_nyabula_core"
rsync -a "$team/app/nyabula_core/" "$dst/packages/demos/contest2026_062_nyabula_core/"
if [ -n "$sqlite_cache" ]; then
  sqlite_cache=$(realpath "$sqlite_cache")
  python3 "$team/app/nyabula_core/prepare_sqlite.py" \
    --output "$dst/packages/demos/contest2026_062_nyabula_core/.sqlite" \
    --cache "$sqlite_cache"
fi
for part in interpreters/quickjs/quickjs interpreters/wamr/wamr crypto/libsodium/libsodium netutils/cjson/cJSON; do
  git init -q "$dst/apps/$part"
done
find "$dst/apps" "$dst/packages" -type f -name Kconfig -exec sed -i "s#$src/#$dst/#g" {} +
mkdir -p "$dst/nuttx/boards/sim/sim/configs/nyabula_core"
cp "$team/configs/nyabula_core_sim/defconfig" \
  "$dst/nuttx/boards/sim/sim/configs/nyabula_core/defconfig"
mkdir -p "$dst/nuttx/boards/arm64/qemu/qemu-armv8a/configs/nyabula_delivery"
cp "$team/configs/nyabula_core_arm64_kernel/defconfig" "$dst/nuttx/boards/arm64/qemu/qemu-armv8a/configs/nyabula_delivery/defconfig"
printf 'PREPARED %s\n' "$dst"
