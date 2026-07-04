# k7_sdpack — KICKPI-K7 (RK3576) SD 启动卡打包

把编译出的 `nuttx.bin` 打成从 SD 卡启动的镜像，NuttX 作 **BL33** 跑在 Rockchip 启动链里。
**eMMC 上的原厂 Android 完全不动**——SD 优先启动，砖了拔卡即回 Android。

启动链（2026-07-04 板上实测点亮 NSH）：

```
SD BootROM → idbloader(DDR init + U-Boot SPL) → FIT(atf-1/uboot/atf-2/atf-3/optee 逐段校验)
  → BL31 → OP-TEE → EL3 exit(EL2) → nuttx@0x40200000 → NuttShell (NSH)
```

## 用法

```sh
./fetch_rkbin.sh rkbin                    # 拉官方启动件(~4MB, 支持 PROXY=host:port)
./build_sd.sh <nuttx.bin> rkbin out       # 组出 out/sd_nuttx.img
# 用 balenaEtcher / Win32DiskImager 整盘烧到 SD 卡, 上电抓串口(1500000 8N1)
```

依赖：`dtc` `sgdisk`(gdisk) `dd`。rkbin 自带 `mkimage`/`boot_merger`/`trust_merger`。

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

## 原理：自写最简 FIT

`build_sd.sh` 拆 BL31 elf 三段 → `atf-1/2/3.bin`，自写 `fit.its`：`uboot` 槽 = nuttx@0x40200000，
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
