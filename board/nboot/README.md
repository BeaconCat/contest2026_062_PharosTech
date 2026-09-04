# N-Boot release payloads

This directory receives the verified KICKPI-K7 N-Boot proper binary and DTB
from public `BeaconCat/N-Boot` releases. Updates are proposed by automation as
pull requests; the protected team branch is never written directly.

`RELEASE` pins the source tag. `SHA256SUMS` is verified before installation,
and the updater additionally checks the ARM64 Image and DTB headers. Rockchip
DDR, BL31 and OP-TEE binaries are intentionally not mirrored here.
