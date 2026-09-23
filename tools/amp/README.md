# KICKPI-K7 AMP 离线集成件

> 2026-09-07 已迁移至最新团队基线并完成编译回归，见[INTEGRATION.md](INTEGRATION.md)。
> N-Boot新基线尚未实现AMP启动，以下旧CPU3说明不能用于刷板。

> 当前目标拓扑已变更为四A53运行NuttX、四A72运行Linux，见[TOPOLOGY.md](TOPOLOGY.md)。
> 下文CPU3启动、旧FIT cpu字段等属于待迁移旧实现，不能用于新拓扑上板。

> 2026-09-06复审：当前仅完成部分构建，尚不是可刷发布候选。已确认CPU3被UP
> 入口拒绝、GIC全局初始化缺少AMP所有权隔离、本地RPTUN资源表没有DRIVER_OK
> 更新来源。必须先修复这些可离线验证的阻塞，不能将此前编译成功视为AMP闭环。

本目录保存 K7 双系统 AMP 的可复现输入。目标分配是：openvela 运行在
RK3576 四个A53，仍是产品控制面；Linux 运行在四个A72，仅提供 NPU、ISP、
ASR/TTS 和重媒体计算服务。WiFi、蓝牙、存储等产品外设默认不因启用 AMP
而自动转交 Linux。

## 固定内存契约

| 区域 | 起始地址 | 大小 | 所有者/用途 |
|---|---:|---:|---|
| rpmsg vring | `0x47800000` | 2 MiB | 两端共享，实际 vring 位于前 64 KiB |
| rpmsg DMA pool | `0x47a00000` | 2 MiB | Linux coherent DMA 分配，openvela 显式维护缓存 |
| service shmem | `0x47c00000` | 4 MiB | 后续大块数据通道，当前仅预留 |
| openvela image | `0x4a400000` | 16 MiB | 四A53共享的固件及运行内存 |

地址由 `linux/rk3576-kickpi-k7-amp.dtsi`、openvela `amp/defconfig` 和
`rk3576_rptun.c` 共同约束。修改任何一处后先运行：

```sh
python3 tools/amp/validate_amp_layout.py
```

## 构建顺序

1. 使用完整 openvela manifest，同步 openvela fork 的 OpenAMP 与 libmetal：

   ```sh
   repo sync nuttx_openamp_open-amp nuttx_openamp_libmetal
   ```

   不允许让 NuttX Makefile 回退下载上游原版 tarball；openvela 的 rptun 使用
   了 priority、utilities 等扩展，二者 ABI 不同。
2. 构建 openvela AMP profile：

   ```sh
   ./build.sh ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/amp -j4
   make -C nuttx distclean
   ./build.sh ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/amp \
     --cmake distclean
   ./build.sh ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/amp --cmake -j4
   ```

   CMake build directory必须在defconfig改变后删除；只增量执行`cmake --build`
   不会可靠地重新导入新RAM地址，可能生成入口仍为旧值的假通过产物。
3. 将 `linux/rk3576-kickpi-k7-amp.dtsi` 包含到 K7 Linux 顶层 DTS 的末尾。
   内核配置先复用Rockchip原版`arch/arm64/configs/rockchip_amp.config`，再叠加
   `linux/nyabula_amp.fragment`；不要重新手写一份vendor已经提供的AMP选项集。
4. 在配套K7 vendor kernel源码上构建精简Image和DTB：

   ```sh
   tools/amp/linux/build_kernel.sh \
     /path/to/kernel-6.1 out/linux rk3576-kickpi-k7-android.dts
   ```
5. 准备静态 BusyBox，生成最小 initramfs：

   ```sh
   tools/amp/nyampd/build_arm64.sh out/nyampd-arm64
   tools/amp/linux/fetch_busybox_static.sh out/busybox-arm64
   tools/amp/linux/build_minimal_initramfs.sh \
     out/busybox-arm64 out/nyabula-amp-initramfs.cpio.gz \
     out/nyampd-arm64/nyampd
   ```
6. 从四个已验证输入生成统一、可重复的external-data FIT：

   ```sh
   tools/amp/nboot/build_amp_fit.sh \
     out/linux/arch/arm64/boot/Image \
     out/linux/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-nyabula-amp.dtb \
     out/nyabula-amp-initramfs.cpio.gz \
     out/openvela/nuttx.bin out/nyabula-amp.itb
   ```

## 已验证与待上板边界

- Make 与 CMake 均需编译到固定入口 `0x4a400000`；这是离线构建门禁。
- mailbox v2 寄存器行为和 Linux `rockchip_rpmsg_mbox` 数据结构已逐行对照；
  真正的中断方向、BL31 AMP SIP、CPU3 隔离及双向 rpmsg 仍必须上板验证。
- Linux 使用 uncached vring 和 coherent DMA pool；openvela profile 启用
  `CONFIG_OPENAMP_CACHE` 做 clean/invalidate。硬件缓存一致性仍以压力板测为准。
- DTS 禁用 Linux 的 CPU3。启用该片段前必须确认启动 DT 确实是改后的 DTB，
  否则 Linux SMP 和 openvela 会同时占用 CPU3。
- Linux PID1会监督`nyampd`；openvela运行`nyampctl health`创建`rpmsg-raw`
  channel并完成首个服务往返。两端都不会把unsupported服务伪报为成功。
