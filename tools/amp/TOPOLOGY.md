# K7 AMP 四小核/四大核拓扑

2026-09-06 用户确定：四个A53归NuttX，四个A72归Linux。本文优先于此前CPU3单核文档。

| 系统 | 物理MPIDR affinity | OS主核 | 核数 |
|---|---|---|---:|
| NuttX | 0x000、0x001、0x002、0x003 | 0x000 | 4 |
| Linux | 0x100、0x101、0x102、0x103 | 0x100 | 4 |

NuttX复用通用ARM64 SMP启动，实现arm64_get_mpid()仅映射LITTLE四核。
Linux禁用全部cpu_l节点并启用全部cpu_b节点。构建后检查展开的.config和最终DTB，
不能只检查defconfig文本：ARCH_HAVE_MULTICPU缺失会让SMP=y被Kconfig丢弃。

## 必须同时完成的启动修改

当前bootamp把Linux留在N-Boot当前小核上运行，不能用于本拓扑。
需要让N-Boot准备Linux入口和DTB，通过跨簇启动向0x100交接Linux，并将当前
小核向NuttX入口交接；两个系统各自启动本簇其余三个核。
Linux初始x0必须是最终DTB地址，x1..x3和EL/cache状态满足ARM64启动协议。
直接把旧命令的cpu=3改成0会对运行N-Boot的核执行CPU_ON，属于错误实现。

GIC distributor只能有一个管理方；另一个OS不得重置全体SPI。Linux的路由目标
也必须使用物理大核target mask，不能假设逻辑CPU0等于物理小核0。
需重新确定mailbox/link-id与物理CPU关系，不能默认旧0x03代表新拓扑。

## 当前完成边界

- 已实现四小核SMP配置、物理CPU映射和超过四核的编译护栏。最终CMake构建通过；
  Make和全新目录CMake均通过，入口`0x4a400000`，bin分别487424、491520字节。
  四核真实运行仍待板测。
- 已调整Linux四大核CPU节点，校验器检查最终DTB八个节点和展开的SMP配置，
  以及MM_REGIONS=1、独立RAM范围、关闭standalone DMA heap。
- FIT打包必须提供同批构建的.config；新字段为Linux cpu=0x100、openvela cpu=0。
  旧bootamp要求cpu=3，会拒绝新FIT，不可改回3绕过。
- 主机13组拓扑/打包回归通过，含实际dtc编译测试树和mkimage打包。测试输入是
  人工fixture，只证明校验逻辑；不代表完整K7 DTB或双系统已经运行。
- 跨簇N-Boot交接、GIC隔离、RPTUN readiness仍在修复范围内。
- 当前产物不放行板刷；上述启动链共同验证后才能生成候选镜像。

## 后续修复顺序和现有实现复用点

1. N-Boot：先复用bootm生成Linux最终DTB和入口，再适配Rockchip现有非启动核
   Linux交接机制。Android参考`u-boot/drivers/cpu/rockchip_amp.c`已有
   `load_linux_for_nonboot_cpu()`和SIP参数传递，不能只调用普通CPU_ON后假定x0是DTB。
   必须先做FIT边界检查，再允许FINDOTHER向目标地址写入。
2. GIC：Linux现有`CONFIG_ROCKCHIP_AMP`已支持AMP IRQ路由和归属检查；
   NuttX现有`arm64_gic_initialize()`仍无条件让逻辑CPU0初始化整个distributor。
   需要单一初始化方、可证明的ready顺序、本地CPU interface初始化和私有SPI清单。
   两边同时启动时“跳过一次初始化”不等于完整的重启隔离。
3. RPTUN：Linux私有mailbox驱动没有共享resource table，`set_status()`为空；
   初始virtqueue notify可用作握手输入，但要覆盖先到通知、丢失kick和重新初始化。
   NuttX本地DRIVER_OK不能开机直接置位来掩盖握手缺失。
4. 以上离线门禁完成后，再统一验证匹配同一Linux源码版本的K7 DTB、时钟/电源/
   外设归属、最终镜像布局和最小板测。当前不承诺Linux可独立重启而NuttX不受影响。
