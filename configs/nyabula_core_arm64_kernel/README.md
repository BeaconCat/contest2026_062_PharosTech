# Nyabula Core ARM64 kernel-build profile

该配置基于上游 `qemu-armv8a:knsh`，用于验证 Nyabula Core 作为独立
AArch64 用户进程运行在 NuttX kernel build 的私有地址环境中。

它刻意关闭包签名与 WAMR：签名、撤销和双运行时功能由
`nyabula_core_sim` 安全配置覆盖；本配置只隔离验证 QuickJS/Core 用户态
ELF，避开上游 libsodium 与 WAMR 的 kernel-import Makefile 缺口。该配置
不能作为生产安全配置。

构建步骤：

```sh
./build.sh qemu-armv8a:nyabula_core_kernel -j4
make -C nuttx export -j1
cd apps
./tools/mkimport.sh -z -x ../nuttx/nuttx-export-*.tar.gz
make -j4
make install -j4
```

预期产物为 `nuttx/nuttx` 内核和 `apps/bin/nycore` AArch64可装载ELF。
运行后执行 `nycore isolation`；只有输出 `build=kernel current_el=0` 才能
作为EL0运行证据。
