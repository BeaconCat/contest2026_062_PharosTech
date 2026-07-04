# KICKPI-K7 (RK3576) — M2 Bring-up 实现文档

> 队伍 062 · Pharos Tech · 项目 Nyabula
> 状态：**M2 达成 — 板上 NSH 命令行点亮（2026-07-04）**
> 面向：队员交接（supercat / yunline）。读完应能自己复现从源码到板上 `nsh>` 的全过程，并理解每一处适配为何这么做。

本文分三层：**做了什么（成果）→ 为什么这么做（原理）→ 怎么做（可复现操作）**。所有标“实测”的都在板上跑出过预期结果；标“文档实证”的仅经 TRM/DTS 核对未上板。

---

## 一、成果概览

在 KICKPI-K7（RK3576，4×A72+4×A53，GIC-400/GICv2，4GB LPDDR5）上，让 openvela(NuttX) 作 **BL33** 从 SD 卡启动，一路走完厂方引导链，进入 NuttShell：

```
SD BootROM → idbloader(DDR init + U-Boot SPL) → FIT(6 段全校验通过)
  → BL31 v2.3(ATF) → OP-TEE 3.13 → EL3 exit(SPSR=0x3c9 → 落 EL2)
  → nuttx head.S → C runtime → NuttShell (NSH) nsh>
```

板上交互实测：`uname -a` 正常；`ls /dev` 见 console/ttyS0/null/zero；`sleep 5` 实测壁钟 5.208s（tick 准确）。

**关键点：全程只烧 SD 卡，eMMC 原厂 Android 一字节不动**——SD 优先于 eMMC 启动，SD 是安全沙盒，eMMC 是天然回退。

---

## 二、整体架构与原理

### 2.1 完全 out-of-tree，生产仓零改动

芯片 arch 与板级代码全放在 vendor 侧，用 openvela 的 `ARCH_CHIP_CUSTOM` / `ARCH_BOARD_CUSTOM` 接入，nuttx 公共仓一行不改（参照 `vendor/artinchip` 先例）。队伍仓子目录经 manifest `<linkfile>` 软链进工程：

```
队伍仓                    →  openvela 工作树
chips/rk3576/             →  vendor/rockchip/chips/rk3576/            (芯片 arch)
boards/rk3576/kickpi-k7/  →  vendor/rockchip/boards/rk3576/kickpi-k7/ (板级)
```

好处：调试期只动队伍仓、逐 commit；公共仓押到最后再提 PR。

### 2.2 BSP 血统：改自 rk3399，但必须审血统

openvela 自带完整 rk3399 移植，RK3399 与 RK3576 同族、同 armv8-a、同款 DesignWare UART，故照 rk3399 改地址/GIC 版本/板配置而非从零。

**但 rk3399 移植本身抄自全志 A64，带毒**。M2 卡死的三个根因全部是“照抄 rk3399 的遗留值没改”：中断号、启动时钟、链接地址（详见第三节）。
> 沉淀教训：**移植参考代码前先审其血统，从参考代码直接抄来的值 ≠ 可信值，必须逐一对 TRM/DTS 核实。**

### 2.3 启动链原理：为什么选“NuttX 当 BL33”

板上固件链是 Rockchip 标准 FIT：`idbloader(SPL+DDR) → uboot.img(FIT: U-Boot + BL31 + OP-TEE + DTB)`。U-Boot 在 FIT 里的 `uboot` 节点就是 BL33，加载地址 `0x40200000`。

我们**把 FIT 里的 U-Boot 载荷原位换成 nuttx.bin**（同槽同地址 0x40200000），其余 ATF/OP-TEE/DTB 节点全不动。这样：

- nuttx 加载地址 `0x40200000` == 我们 `CONFIG_RAM_START`，天然对齐；
- SPL→BL31→OP-TEE→跳 0x40200000 进 nuttx，EL 交接干净（实测进 EL2）；
- 不碰 boot 分区、不碰 eMMC，改 nuttx 后一条命令重出卡。

---

## 三、关键实现点（逐个：改了什么 + 为什么 + 在哪）

### 3.1 链接地址（M2 点亮的最后一把钥匙）— 实测

- **症状**：全链贯通、跳 0x40200000 后 nuttx 静默挂死。
- **根因**：`scripts/dramboot.ld` 链接地址从 rk3399 抄来写死 `0x02080000`，而实际加载/跳转在 `0x40200000`。头几条位置无关指令能跑，一遇绝对寻址即飞。ELF 入口实测 `0x2080000 ≠ 0x40200000`。
- **修复**：`dramboot.ld` 链接基址 → `0x40200000`（= `CONFIG_RAM_START`）。重编后 ELF 入口实测变 `0x40200000`，一次点亮。

### 3.2 中断号 irq.h（PR#4）— 文档三方互证

- **根因**：irq.h 照抄 rk3399，而 rk3399 又抄全志 A64，中断号全错。旧 `NR_IRQS=181` 会让 GPIO(185+)/SDMMC(283)/FSPI1(287) 等 `irq_attach` 必失败，M7 外设全瘫。
- **修复**：脚本解析 RK3576 TRM Part1 Table 1-3 提取 374 条映射，重写为 359 个 `RK3576_IRQ_*` 宏，`NR_IRQS` → 416。
- **核对法**：TRM 表号已含 +32（即 GIC INTID），与反编 DTS 交叉验证 `DTS SPI号 + 32 == TRM 表号`：UART0=108✓ FSPI1=287✓ SPI2=150✓ SDMMC=283✓。

### 3.3 启动时钟 arm64_el_init CNTFRQ（PR#5）— 编译验证

- **根因**：`rk3576_boot.c` 的 `arm64_el_init` 是空 stub（抄 rk3399）。若 MiniLoader 直跳 BL33 且处 EL3，`cntfrq_el0=0` → banner 后 tick 死。
- **修复**：EL3 时 `write_sysreg(24000000, cntfrq_el0)+ISB`；EL2/EL1 no-op。这是全部 in-tree arm64 移植的惯例（imx8/imx9/zynq 都只在 el_init 写 cntfrq，无人碰 CRU）。
- **实测旁证**：本板走 BL31（v2.3）路径，ATF 已设 cntfrq，此修复在 EL2 为 no-op；`sleep 5=5.208s` 证 tick 正确。两种交接模式都安全。

### 3.4 内存图 chip.h / defconfig — 实测回填（本次 PR#6 增补）

板上 `/proc/iomem` 实测的真实布局：

```
0x40000000  ┬ BL31 保留 (2MB)
0x40200000  │  ← NuttX 加载/运行 (CONFIG_RAM_START)  ┐
   ...      │     NuttX 堆区 (130MB)                  │ DRAM0 MMU 区
0x48400000  ┤ OP-TEE 保留 (16MB)                     ┘ (132MB, 止于此)
0x49400000  ┴ 大块可用 System RAM (日后 RAMBANK2)
   ...
0x13FFFFFFF ┴ 4GB LPDDR5 物理末端（连续，跨 4G 线不断）
```

据此把占位值回填为板验值（`chips/rk3576/include/chip.h` + `configs/nsh/defconfig`）：

| 项 | 旧（占位/TBD） | 新（板验） | 为什么 |
|---|---|---|---|
| `CONFIG_LOAD_BASE` | 0x40200000 /* TBD */ | **删除** | 全树零引用的死宏，真加载地址是 defconfig `CONFIG_RAM_START`，删掉去重、防误用 |
| `CONFIG_RAMBANK1_SIZE`（MMU 区） | MB(1024) | **MB(132)** | 映射止于 OP-TEE 0x48400000，不越界 |
| `CONFIG_RAM_SIZE`（堆） | 268435456 (256MB) | **136314880** (~130MB) | 堆末原为 0x50200000 会**越入 OP-TEE secure 区**；封到 0x48400000 |

> 注：这些值本就是 07-04 点亮 NSH 所用配置，本次只是把队伍仓里没回填的占位值同步过来。DRAM0 MMU 区从 0x40000000 起（含 2MB BL31 洞），但 NuttX 只在 RAM_START 之上分配，洞被映射但从不触碰。

### 3.5 串口与 GIC — 文档实证 + 实测

- **UART0 @ 0x2AD40000**，DesignWare 16550，`reg-shift=2`，波特率 `1500000`。取自 cmdline `earlycon=uart8250,mmio32,0x2ad40000`（实测原文）。
- **GIC-400 = GICv2**（区别于 RK3588 的 GICv3）：`CONFIG_ARM64_GIC_VERSION=2`，GICD `0x2A701000` / GICC `0x2A702000`。

---

## 四、可复现操作过程

### 4.1 编译

```bash
# openvela 工作区根目录，工具链已在 PATH
cd /root/openvela/src/vela
export PATH=/root/openvela/src/vela/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
./build.sh ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh -j4
# 产出 nuttx/nuttx.bin（ARM aarch64，~420KB）
```

### 4.2 打包 SD 启动镜像（rkbin 官方件 + 自建 FIT）

启动件全部取自 Rockchip 官方二进制仓 [rkbin](https://github.com/rockchip-linux/rkbin)，
**仓内不含任何从设备镜像抠出的私有 blob**（license 干净）。脚本在 `tools/k7_sdpack/`：

```bash
cd tools/k7_sdpack
./fetch_rkbin.sh rkbin              # 拉所需 ~10 个 RK3576 官方件(~4MB, 支持 PROXY=host:port)
./build_sd.sh <nuttx.bin> rkbin out # 组出 out/sd_nuttx.img
# 烧 out/sd_nuttx.img
```

原理（见 `tools/k7_sdpack/README.md`）：拆 rkbin BL31 elf 的 3 个 PT_LOAD 段 → `atf-1/2/3`，
自写最简 `fit.its`（`uboot` 槽 = nuttx@0x40200000 + atf-1/2/3 + optee@0x48400000 + dummy-fdt），
`mkimage` **内嵌**打包（不用 `-E`，各段 hash 由 mkimage 重算、布局自洽）；idbloader、trust 用
rkbin 的 merger 按官方 `.ini` 生成。SD 布局 idbloader@64 / uboot@16384 / trust@24576。

> rkbin BL31 elf 段址 `0x40060000 / 0x400f0000 / 0x3fe70000` 与原厂 vendor FIT 的 atf-1/2/3 逐一
> 吻合，证同族固件；版本 v1.24/OP-TEE v1.08 较原厂新，2026-07-04 板上实测兼容、一次点亮。

> **弃用的旧法（外科手术）**：早期抠 vendor `uboot.img` 原位换 BL33 载荷 + 改一处 hash。因坑多
> （见 5.1）已被 rkbin 自建取代；`make_sd.sh`/`patch_fit_uboot.py` 仅作无 rkbin 时的应急回退。

### 4.3 烧卡 + 抓串口

1. Windows 用 balenaEtcher 把 `烧录/sd_nuttx.img` 烧到 SD 卡。
2. SD 插板，CH340 接 40pin **pin35/37/39**，串口 **1500000 8N1**。
3. 上电，串口应见完整引导链直到 `NuttShell (NSH) nsh>`。

> SD 优先启动，验完拔卡即回原厂 Android，零风险。ADB over Type-C（186MB/s）可作大文件快传通道，免走 1.5M 串口。

---

## 五、踩坑记录（最值钱的部分）

### 5.1 FIT `Bad hash` / atf-3 被覆盖（已定性）
手术改 vendor FIT 时用 mkimage `-E`（外部数据）+ 载荷改尺寸 → 段 data-position 位移 → SPL 加载缓冲落点与 atf-3(0x3fe70000, SRAM 低地址)重叠自覆盖 → 两次坏 hash 值还不同（非确定性=运行时被改写）。**根因是"改 vendor FIT"本身**，非 mkimage 之过。→ 改用 rkbin 官方件 + 干净内嵌 FIT（4.2），mkimage 重算各段 hash、布局自洽，atf-3 一次通过，坑随之消失。

### 5.2 隔 SSH 写含 `\x00` 的脚本被截断
隔多层 SSH 用 `python3 -c` 写含 NUL 的字节串 → shell 截断 → 脚本残且被认成 binary。→ **复杂脚本本地 Write 干净版再 scp；补零用 `truncate` 不用 python 字节串**。

### 5.3 python 脚本插宏插进 header guard 中间
以首个 `#define` 为锚插宏，插进了 `#ifndef`/`#define` guard 之间，guard 破坏。→ **插宏锚点用 guard `#define` 之后**。

### 5.4 重写 chip.h 漏 get_cpu_id 汇编宏
head.S 依赖，漏了报 `unknown mnemonic`。→ 重写 chip.h 保留 `__ASSEMBLY__` 段的 `get_cpu_id` 宏。

---

## 六、当前状态与后续

- **已达成**：M2（NSH 点亮）。三个抄 rk3399 遗留（irq.h / cntfrq / dramboot 链接地址）全清。
- **启动件官方化（实测）**：整条链改从 rkbin 官方件全量复现，仓内零设备抠出 blob，`tools/k7_sdpack/` 自建 FIT 板上一次点亮 NSH。
- **两仓一致**：队伍仓（github，PR#6）与 VM vendor 树（gitee）内存图已同步。
- **后续（M3~M7）**：时钟树/CRU、GIC 中断实触发、MMU 细分、SMP 多核、外设驱动（UART 完整/SPI/I2C/GPIO/双圆屏 QSPI）。
- **相关 PR**：#4 irq.h、#5 cntfrq、#6 链接地址修复 + 内存图回填 + rkbin 打包套件。

---

*本文所有值以 `实测日志.md` 为事实底座；如与代码不符，以板上实测为准并回填本文。*
