# RK3576 / KICKPI-K7 USB-A 移植研究

## 目标

将 KICKPI-K7 的三个 USB-A 接口接入 OpenVela/NuttX USB host 栈，优先复用
NuttX 已有 xHCI、USB Hub 和 class driver，只实现 RK3576 平台缺口及经实证确认
的通用 xHCI 缺口。

## 已确认的硬件拓扑

- RK3576 USB3_OTG1/DWC3 基址为 `0x23400000`，寄存器窗口大小
  `0x400000`。
- DWC3 host IRQ 为 GIC SPI 260，即 NuttX 绝对 IRQ 292。
- USB-A 使用 USB2 PHY1 和 Combo PIPE PHY1；DTS 将控制器设为 host mode，并
  标记 DMA coherent。
- K7 USB-A VBUS 由 `GPIO3_D6` 高电平使能，DTS regulator 名为
  `vcc5v0_host`。
- K7 V2.1 原理图第 22 页确认外部 Hub 为 Genesys Logic GL3510-QFN64。
  上游同时连接 USB2_HOST1 DP/DM 和 USB3_HOST1 SuperSpeed lane；下游 DS1、
  DS2、DS3 接三个 USB-A，DS4 继续路由到板载 4G/mini-PCIe 接口。因此完整
  实现需要四口 USB2/USB3 companion Hub，而不只是三个外部插座。
- Debian 黄金串口日志给出运行时身份：
  - USB2.1 Hub：`05e3:0610`，四口。
  - USB3.1 Hub：`05e3:0626`，四口。

证据：

- Android 原版
  `kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576.dtsi`
- Android 原版
  `kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7.dtsi`
- 本地反编 DTS `4-HardwareData/k7_debian_vendor.dts`
- K7 V2.1 原理图：RK3576 PCIe/SATA/USB3 Combo PHY 与 GPIO3_D6

## OpenVela/NuttX 可复用能力

NuttX 已有：

- 完整 xHCI capability/operational/runtime/doorbell 寄存器访问。
- Command/Event/Transfer ring、slot、endpoint、control/bulk/interrupt/isoc
  传输及 cache maintenance。
- USB host waiter、枚举框架和 MSC、HID、CDC、Bluetooth 等 class driver。
- 通用 USB Hub class driver。

团队主线还已有 `chips/rk3576/rk3576_usb.c`：

- USB0 DWC3 gadget/ADB 已经板测使用。
- 已实现 DWC3 core/PHY soft reset、UTMI wide、禁用 U2 free clock 等
  RK3576 quirk。
- 已验证 USB2 PHY0 GRF 与 USB_GRF 的 USB2-only device-mode 配置。

USB1 host 必须复用其中通用 DWC3 生命周期与寄存器定义，但不能照搬 USB0
专属的 USB2-only/缺失 USBDP PHY workaround；USB1 需要真正启用 Combo PHY1
和 SuperSpeed。

当前 xHCI 实现的硬件入口仅为 PCI：

- 文件为 `drivers/usbhost/usbhost_xhci_pci.c`。
- 初始化依赖 PCI BAR、MSI-X 和 PCI IRQ 分配。
- 数据面的大部分逻辑与 PCI 无关，适合抽成通用 xHCI core，PCI 和 platform
  分别提供 MMIO/IRQ 生命周期。

## 已确认的缺口

1. 缺少 platform/MMIO xHCI 初始化入口。
2. 缺少 RK3576 DWC3 host-mode、clock/reset、USB2 PHY、Combo PHY 与 VBUS
   初始化。
3. 当前 xHCI 明确禁止 `CONFIG_USBHOST_HUB`，但文件中已经存在部分 Hub
   结构和 TT 字段；route string、Hub slot context 和 Hub connect 回调仍未实现。
4. 因 K7 三个 USB-A 依赖外部 Hub，只实现根端口不能视为完成。

## 实现边界

- 不重写 USB 枚举、xHCI ring、MSC/HID/CDC 等已有协议层。
- 将 PCI xHCI 中与总线无关的逻辑抽成通用 core；保持 PCI 行为不变，并增加
  platform 初始化 API。
- RK3576 文件只拥有芯片寄存器事实、PHY/clock/reset 和 DWC3 glue。
- 保持现有 USB0 gadget/ADB 行为不变；公共 DWC3 代码的抽取必须先后通过
  USB0 构建与板上 ADB 回归。
- K7 board 文件只拥有 GPIO3_D6 VBUS、class 注册和 host waiter 生命周期。
- Hub 支持在通用 xHCI 层实现并以 K7 板载 Hub 实测，不写 K7 特判。

## 首轮验收

1. Make 与 CMake 均构建通过。
2. 控制器读取合法 xHCI capability，完成 reset/run 并收到 port-change IRQ。
3. 枚举板载 USB2/USB3 Hub，记录 VID/PID、层级和端口数。
4. 三个 USB-A 分别完成插拔。
5. USB2 U 盘完成枚举、挂载、读写和拔出；USB3 U 盘确认 SuperSpeed。
6. 键盘/鼠标完成 HID 输入。
7. Hub 下设备反复插拔后 WiFi、Bluetooth、ADB 和系统调度保持正常。

## 当前置信度

- 地址、IRQ、PHY、VBUS GPIO：源码/原理图确认，尚未在 OpenVela 板上验证。
- NuttX xHCI/Hub 缺口：源码确认。
- GL3510 与四个下游连接、VID/PID和USB2/USB3 companion枚举：Linux实测
  确认；OpenVela枚举仍待验证。
