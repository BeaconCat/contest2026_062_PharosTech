# N-Boot release payloads

This directory receives the verified KICKPI-K7 N-Boot proper binary and DTB
from public `BeaconCat/N-Boot` releases. Updates are proposed by automation as
pull requests; the protected team branch is never written directly.

`RELEASE` pins the source tag. `nboot-release.json` pins the source commit and
KICKPI-K7 load/layout contract. `SHA256SUMS` and manifest artifact records are
both verified before installation. Rockchip DDR, BL31 and OP-TEE binaries are
intentionally not mirrored here.
