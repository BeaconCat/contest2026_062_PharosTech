# RK3576 Ultimate BSP for KICKPI-K7

Pharos Tech（队伍 062）为 KICKPI-K7 / RK3576 开发的 openvela BSP 集成分支。
本分支用于集中保存和编译验证尚未拆分上游的芯片、板级与产品实验代码；成熟功能会按驱动拆成独立 PR。

> 分支：`rk3576-ultimate-bsp`
> 基线：openvela `dev-ai-contest-2026`
> 当前置信度：基础 BSP 与部分外设已上板；本分支新增驱动多数仅完成编译验证，不能据此视为硬件可用。

## 快速开始

使用队伍 manifest 获取完整工程，不要单独 clone 本仓代替 `repo`：

```bash
repo init \
  -u https://github.com/BeaconCat/contest2026_062_PharosTech \
  -b rk3576-ultimate-bsp \
  -m contest2026_062_PharosTech.xml
repo sync -c -j4

cd <openvela-workspace>
export PATH="$PWD/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH"
./build.sh vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh -j4
```

产物：

- `nuttx/nuttx`：带符号 ELF
- `nuttx/nuttx.bin`：RK3576 BL33 裸二进制

生成 KICKPI-K7 FIT 和整卡镜像：

```bash
vendor/rockchip/tools/k7_sdpack/build_sd.sh \
  nuttx/nuttx.bin /path/to/rkbin /path/to/output
```

## 如何确认不是“假编译通过”

退出码为 0 只能说明当前配置完成了链接。新增驱动必须同时检查 Kconfig 和归档符号：

```bash
grep '^CONFIG_RK3576_' nuttx/.config

aarch64-none-elf-nm -g --defined-only \
  nuttx/arch/arm64/src/libarch.a | grep ' T rk3576_'
```

本分支默认 `nsh` 配置启用一组编译回归驱动：power-domain、pinctrl、mailbox、OTP、RNG、watchdog、SPI、I3C、SARADC、TSADC 和 IR。链接器可能从最终 ELF 删除未被初始化路径引用的函数，因此应以 `libarch.a` 验证编译收录，以最终 ELF 验证运行时集成。

## 已上板基础能力

| 模块 | 状态 | 说明 |
|---|---|---|
| 启动、MMU、GICv2、PSCI | 实测确认 | MiniLoader → BL33，NSH 可用 |
| UART0 | 实测确认 | DW16550，1.5 Mbaud |
| GPIO / IOMUX | 实测确认 | 5 个 bank，中断支持 |
| I2C | 实测确认 | 10 个控制器，HYM8563 实测 |
| SDMMC | 实测确认 | PIO + IDMAC，GPT/FAT |
| eMMC | 实测确认 | dwcmshc / SDHCI |
| PL330 DMA | 实测确认 | 8 通道，音频链路使用 |
| USB DWC3 gadget | 实测确认 | CDC-ACM 与 ADB |
| PWM | 实测确认 | Rockchip PWM v4 |
| SAI + ES8388 | 实测确认 | 32/44.1/48/96 kHz 播放链路另行维护 |

## Ultimate BSP 驱动集合

### 基础与低速外设

- 电源域、pinctrl、外设时钟树、SMP CPU boot、mailbox
- watchdog、SPI、OTP、TRNG、SARADC、TSADC、I3C、IR、PDM、Crypto

### 网络与高速外设

- GMAC ×2
- PCIe Root Complex 与 Combo PHY
- SeekWave SV6621 相关实验代码

### 显示、多媒体与摄像头

- VOP2、HDMI TX、RGA、RKVDEC
- CSI-2 host、D-PHY、VICAP、ISP
- RK806、HYM8563、HUSB311、IMX415 板级驱动

### AI 与图形

- RKNPU 实验驱动与 RKNN matmul
- Mali-G52 Job Manager 实验驱动
- `app/llm/` CPU/NPU 可切换推理实验
- `app/softgl/` CPU 软件光栅化实验

其中大量模块仍处于“编译通过”或“仅推断”阶段。请查看代码、提交历史和项目实测日志，不要把驱动存在等同于硬件完成验证。

## 重要风险

### 时钟 Gate 冲突

部分多媒体、PCIe、GMAC、NPU 时钟定义来自尚未完全裁决的资料，多个驱动曾声明相同 gate 位。错误地执行 `clk_disable()` 可能关闭其他模块并造成系统假死。未完成 TRM/DTS/板上交叉验证前，不要同时启用这些高风险驱动。

RK3576 没有 NPLL。可用 PLL 为 BPLL、VPLL、AUPLL、CPLL、GPLL，以及 PMU 域 PPLL。

### 编译不等于上板可用

状态定义：

- 实测确认：开发板运行得到预期结果
- 编译通过：构建和符号检查通过，但未完成板上验证
- 仅推断：根据 TRM、DTS 或同类驱动实现，尚未验证

提交或评审时必须明确标注，不得把推断写成事实。

## 建议验证顺序

1. Watchdog：验证时钟、IRQ 和复位路径
2. SPI：双屏 QSPI 的前置能力
3. Mailbox：AMP / RPMsg 地基
4. OTP：读取唯一 ID，供网络地址生成
5. RNG：验证随机数质量与 NuttX devrandom 接口
6. Mali：先完成上电和 GPU ID 读取
7. 最后处理存在 gate 冲突的多媒体、网络和 NPU 模块

每完成一个功能节点，再整理为独立提交或 PR；不要把未验证的 Ultimate BSP 整包直接提交上游。

## 目录

```text
boards/rk3576/kickpi-k7/  板级配置、初始化和板载器件
chips/rk3576/             RK3576 芯片驱动
app/llm/                  LLM 推理实验
app/softgl/               软件 3D 光栅化实验
tools/k7_ota/             ADB/Ymodem 热更新工具
tools/k7_sdpack/          FIT 与 SD 镜像打包工具
```

## 协作约定

- 贡献代码的 C/H 注释、syslog 和调试字符串使用英文。
- 公共 API 放在模块公共头中，禁止散落手写原型。
- 文件使用完整 SPDX + Apache-2.0 banner，头文件必须自包含。
- 初始化进入 `board_late_initialize()`，不放入 `board_app_initialize()`。
- 提交前运行 clang-format，并检查无冲突标记、无构建产物、无失效 TODO。
- 硬件验证结果同步记录到项目 `实测日志.md`，同时标记置信度。

## 当前构建记录

2026-08-02 在 Debian 13 / `aarch64-none-elf-gcc 13.4.0` 环境，以 `-j4` 对本分支执行干净构建，修复 SARADC 缺少 ADC ioctl 公共头后通过：

- `nuttx/nuttx`：7,469,712 字节
- `nuttx/nuttx.bin`：634,880 字节
- 上述 11 个回归驱动均保持 `CONFIG_RK3576_*=y`
- `libarch.a` 中确认存在对应定义符号；按模块前缀统计共 42 个

这项记录的置信度为“编译通过”，不是新增驱动的板上实测结论。
