# k7_pack — KICKPI-K7 SD/eMMC发布打包

使用同一份N-Boot、bootctrl和固定分区合同，生成SD整盘镜像或RKDevTool eMMC
分区包。构建脚本只在团队仓维护，N-Boot仓仅发布proper与控制DTB。

启动链（2026-07-04 板上实测点亮 NSH）：

```
SD BootROM → idbloader(DDR init + U-Boot SPL) → FIT(atf-1/uboot/atf-2/atf-3/optee 逐段校验)
  → BL31 → OP-TEE → EL3 exit(EL2) → nuttx@0x40200000 → NuttShell (NSH)
```

## 用法

```sh
./fetch_rkbin.sh rkbin                    # 拉官方启动件(~4MB, 支持 PROXY=host:port)
./build_sd.sh <nuttx.bin> ../../board/nboot rkbin out-sd
./build_emmc.sh <nuttx.bin> ../../board/nboot rkbin out-emmc
```

SD产物为`out-sd/nyabula-k7-sd.img`，使用balenaEtcher或Win32DiskImager整盘
写入。eMMC产物位于`out-emmc/nyabula-k7-emmc/`，包含：

```text
package-file
SHA256SUMS
README.txt
Image/MiniLoaderAll.bin
Image/parameter.txt
Image/uboot.img
Image/trust.img
Image/bootctrl.img
Image/nuttx_a.img
Image/nuttx_b.img
```

RKDevTool使用Download Image模式：加载`MiniLoaderAll.bin`作为Loader，加载
`parameter.txt`创建GPT，再按同名项刷入各分区镜像。`amp_a`、`amp_b`和自动扩展
到设备末尾的`data`只建分区，本阶段不写初始内容。

两个入口都调用内部`build_nboot_ab.sh`，因此N-Boot FIT、bootctrl、分区起始和大小
不会形成两套实现。相同输入和`SOURCE_DATE_EPOCH`应生成逐字节一致的payload。

依赖：`dtc` `sgdisk`(gdisk) `dd` `mkfs.fat`。rkbin 自带
`mkimage`/`boot_merger`/`trust_merger`。

## 启动件全部来自官方 rkbin —— 不含设备抠出的私有 blob

引导件（DDR/SPL/BL31/OP-TEE）全部取自 Rockchip 官方二进制仓
[rockchip-linux/rkbin](https://github.com/rockchip-linux/rkbin)，本目录不提交任何从设备
镜像抠出的专有 blob（license 干净，可随 Apache 仓分发）。`fetch_rkbin.sh` 只下载所需的
~10 个文件。

> **rkbin revision 固定**：`fetch_rkbin.sh` 里 `REV` 钉死在一个具体 commit
> （`ecb4fcbe`，2025-12-30，板上实测所用），**不跟 `master`**——rkbin 无发布 tag 且更新可能
> 破坏兼容性。需要升级时手动改 `REV` 并**重新上板验证**。

固定版本（与 rkbin 的 `.ini` 一致）：

| 件 | rkbin 文件 | 用途 |
|---|---|---|
| DDR | `rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.12.bin` | idbloader（LPDDR5 2736MHz，板实测同频） |
| SPL | `rk3576_spl_v1.08.bin` | idbloader（== 板原厂 SPL 同版 v1.08） |
| BL31 | `rk3576_bl31_v1.24.elf` | FIT 的 atf-1/2/3 三段 + trust |
| BL32 | `rk3576_bl32_v1.08.bin` | FIT 的 optee 段 + trust |

> rkbin BL31 elf 的 3 个 PT_LOAD 段载入址 `0x40060000 / 0x400f0000 / 0x3fe70000` 与原厂
> vendor FIT 的 atf-1/2/3 逐一吻合，证实同族固件；仅版本较新（v1.24 vs 原厂 v1.20），实测兼容。

## 旧版NuttX直作BL33镜像

早期、不含N-Boot A/B的64 MiB开发镜像保留为：

```sh
./build_legacy_sd.sh <nuttx.bin> rkbin out
```

仅用于复现历史bring-up，不作为产品发布路径。

## 原理：自写最简 FIT

`build_legacy_sd.sh`拆BL31 elf三段，自写`fit.its`：`uboot`槽为nuttx@0x40200000，
外加 atf-1(firmware)/atf-2/atf-3/optee@0x48400000/dummy-fdt，`mkimage` 内嵌打包成 `uboot_nuttx.img`。
idbloader、trust 用 rkbin 的 merger 按官方 `.ini` 生成。

SD 布局（Rockchip 标准扇区偏移）：

| 扇区 | 内容 |
|---|---|
| 64 | idbloader（DDR + SPL） |
| 16384 | uboot_nuttx（FIT，uboot 槽 = nuttx） |
| 24576 | trust（BL31 + BL32） |

## 为何不用 mkimage 重打 vendor FIT（历史坑）

早期方案：抠原厂 `uboot.img` 做外科手术替换 BL33。踩坑：mkimage `-E`（外部数据）+ 载荷改尺寸
→ 各段 data-position 位移 → `atf-3@0x3fe70000`(SRAM) 落点被 SPL 加载缓冲覆盖 → 运行时 `Bad hash`
→ SPL 回退崩溃。

**现方案**用干净内嵌 FIT（无 `-E`，各段 hash 由 mkimage 重算，布局自洽），atf-3 一次通过，
无需填充/手术。这也是弃用设备抠出件的直接收益。

## 为何不签名

硬件实测 SPL 打印 `Verified-boot: 0` → 不强制验签；且未持 vendor 私钥，自签 SPL 也不认。
故不做签名。
