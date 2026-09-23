# KICKPI-K7 AMP 最小集成

2026-09-10已上板跑通：四A53运行openvela，四A72运行Linux，NSH通过真实
RPMsg调用nyampctl health和nyampctl info。本分支基于团队PR #83。
配套N-Boot需要新bootamp命令，并以N-Boot PR #7为基线。

本地槽启动分支已增加活动AMP槽试启动与回退，见[SLOT_BOOT.md](SLOT_BOOT.md)。
已验证固定INT8矩阵乘的真实NPU请求闭环；完整模型、独立重启、产品外设
仍不在最小基线内，也不是安全隔离。
详见[所有权](OWNERSHIP.md)、[板测结果](BOARD_VALIDATION.md)和[基线](INTEGRATION.md)。

AMP二进制计划由独立镜像仓发布，本仓不提交每轮FIT或第三方运行库。
目前没有公开发布地址。下载、版本固定、CI接入和许可证边界见
[DISTRIBUTION.md](DISTRIBUTION.md)。带RKNN的当前产物仅供本地验证，
再分发许可尚未核清，不是可直接公开上传的发行包。

## 内存与CPU

| 用途 | 地址范围 |
|---|---|
| Linux Image | 从0x42000000加载 |
| RPMsg vring | 0x47800000..0x47a00000 |
| RPMsg DMA pool | 0x47a00000..0x47c00000 |
| 预留服务共享区 | 0x47c00000..0x48000000 |
| OP-TEE（不占用） | 0x48400000..0x49400000 |
| openvela | 0x4a400000..0x4b400000 |

Linux DTB只保留MPIDR 0x100..0x103的四个A72节点，不能只把A53标为disabled。
N-Boot启动大核Linux，等待GIC与RPMsg首个通知，然后当前CPU0进入openvela。
两OS分别启动自己簇内另外三个核。FIT契约版本为ABI2。

## 构建

使用完整openvela repo工作区；OpenAMP/libmetal必须来自openvela manifest。
以下在工作区根目录执行：

```sh
TEAM=contest2026_062_PharosTech
bash "$TEAM/tools/amp/nuttx/apply.sh" nuttx
./build.sh "$TEAM/boards/rk3576/kickpi-k7/configs/amp" -j4
```

GIC补丁基于NuttX e02f581e235fc7b527d57ff62b668ce625d139ab，暂未提交公共仓。
脚本检查后应用，拒绝冲突，应在独立构建工作区使用。CI仅对AMP配置应用；
普通配置不启用新选项。切换配置须重新配置，二进制与展开.config必须同批。

以下在团队仓根目录执行。Linux Image与DTB必须使用同一K7 SDK；实测为
Android14 SDK b553a938ddb56541f86507050f51af76c9929beb的kernel-6.1子树。

```sh
bash tools/amp/linux/build_kernel.sh /path/to/k7/kernel-6.1 out/linux
bash tools/amp/nyampd/build_arm64.sh out/nyampd-arm64
bash tools/amp/linux/fetch_busybox_static.sh out/busybox-arm64
bash tools/amp/linux/build_minimal_initramfs.sh out/busybox-arm64 \
  out/initramfs.cpio.gz out/nyampd-arm64/nyampd
bash tools/amp/nboot/build_amp_fit.sh \
  out/linux/arch/arm64/boot/Image \
  out/linux/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-nyabula-amp.dtb \
  out/initramfs.cpio.gz /path/to/nuttx.bin /path/to/.config out/amp.itb
python3 tools/amp/test_amp_topology.py
```

### 可选NPU后端

外部SDK目录应包含`include/rknn_api.h`、`include/rknn_matmul_api.h`和
Linux AArch64版`lib/librknnrt.so`。不得使用Android/Bionic版运行库。
本地验证使用rknn-toolkit2 v2.3.2；不传SDK路径则保持静态health/info版。

```sh
bash tools/amp/nyampd/build_arm64.sh out/nyampd-rknn /path/to/rknn-sdk
bash tools/amp/linux/build_minimal_initramfs.sh out/busybox-arm64 \
  out/initramfs-npu.cpio.gz out/nyampd-rknn/nyampd \
  out/nyampd-rknn/runtime-libs
```

用同一`build_amp_fit.sh`打包新的initramfs，再按SLOT_BOOT.md热更新AMP槽。
NSH执行`nyampctl npu 1`：固定M=1/K=64/N=32，返回32个INT32结果，
openvela逐项独立校验；`setup_us`为初始化时间，`run_us`为SDK阻塞执行时间，
不等于端到端延迟。当前不支持模型文件、LLM、批量异步任务或CPU回退。

## RAM启动

通过nbootctl update-nboot更新配套N-Boot的完整4MiB vendor FIT并回读校验；
不能将裸proper写入启动槽。保留已知好启动器备份和普通NuttX槽。

N-Boot执行fastboot usb 0，主机执行fastboot stage out/amp.itb，串口发送单ETX
退出。下载地址0x60000000。命令以单CR结尾，CRLF会多发空命令。

```text
bootamp 60000000 <FIT字节数的十六进制> check
bootamp 60000000 <FIT字节数的十六进制>
```

NSH中运行nyampctl health与nyampctl info。不得把AMP FIT写入普通nuttx_a/b，
那些槽按0x40200000启动，与AMP openvela入口不同。
