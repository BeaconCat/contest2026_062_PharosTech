# AMP 资源所有权与启动门禁

2026-09-07。目标契约，尚未全部实现；不是板测通过声明。

## 初始化与运行必须分开审查

| 资源 | 目标管理方 | 另一个系统的约束 |
|---|---|---|
| GIC distributor 全局初始化 | Linux | NuttX不得重置SPI或重写GICD_CTLR，包括次核启动 |
| SGI/PPI banked寄存器、CPU interface | 各自CPU所属OS | 只初始化本簇CPU，禁止跨簇SGI目标 |
| 私有SPI使能/屏蔽 | 该IRQ所属OS | 使用W1S/W1C；不能清理整个共享寄存器 |
| SPI优先级、目标核、触发模式 | 初始化时Linux统一配置 | 运行期不能以本OS自旋锁保护跨OS的共享word读改写 |
| CPU0..3 | NuttX | Linux DTB全部禁用A53 |
| CPU0x100..0x103 | Linux | NuttX的MPIDR映射只支持A53 |
| UART0 | NuttX产品串口 | Linux不得同时运行普通UART驱动、FIQ debugger或earlycon |
| Mailbox0/3及共享vring | 固定双端协议 | 两端分别管理发送/接收方向，不能重置对端仍使用的实例 |
| DDR/共享父时钟/电源域 | 需要集中控制，尚待具体清单 | 不能因Linux unused-clock清理、DVFS或suspend破坏NuttX |
| SD/eMMC启动介质 | 启动期N-Boot；运行期另行分配 | 最小Linux保持RAM-only，不允许两个OS同时实例化同一host |

该表描述协作契约，不是硬件访问控制。当前NuttX MMU还映射广泛DDR/设备区域，
不能宣称一个失控内核无法破坏另一个系统。

## 本轮已实现：初始化不破坏共享GIC

NuttX专用工作树`tmp/nuttx-amp-gicv2-20260907`，基于构建内核
`e02f581e235fc7b527d57ff62b668ce625d139ab`。

新增默认关闭的`CONFIG_ARM64_GICV2_PREINITIALIZED`：

- 跳过主核对所有SPI分组、禁用、触发、优先级、目标核寄存器的初始化写入。
- 跳过所有核初始化时对GICD_CTLR的写入。
- 保留私有SGI/PPI及CPU interface初始化。
- 保留SMP的调度/调用SGI处理器注册，不能把它们随全局初始化一起跳过。
- 目前限定非secure、非GICv2m配置；其他配置维持旧路径。

主机测试直接编译真实`arm64_gicv2.c`，仅替换依赖头和MMIO后端，不复制初始化逻辑。
旧源码在UP/SMP预初始化测试中均复现全局写入；新源码四种组合均通过，含三个
次核及错误GIC版本的无写入退出。完整Make/CMake开启该选项的四核测试配置通过。

测试配置仅位于独立构建目录`configs/amp_gic_test`，**未启用到产品amp/defconfig**。
公共内核修改未推送，团队manifest也尚未切换依赖。不能在旧内核上写入这个未知
选项后，忽略Kconfig丢弃并宣称安全；未来启用时必须检查展开.config。

## 仍然不能放行启动的条件

1. **Linux-ready顺序**：N-Boot必须区分本次启动与上次留下的寄存器状态。
   GICD_CTLR非零只能说明某时刻被启用，不证明当前Linux已完成配置；不能把它
   当作单独的ready条件。需明确每轮初始化/发布/等待/超时流程，之后才启用上述选项。
2. **运行期共享word写入**：新增独立的默认关闭选项`ARM64_GICV2_STATIC_SPI`，
   依赖PREINITIALIZED；冻结SPI优先级、路由、触发模式。优先级/触发请求与现有
   配置一致时返回0，改变时返回EPERM；affinity的void API保留原路由，冲突请求
   仅输出debug提示。私有PPI和逐IRQ启停保持可用。主机UP/SMP六种组合通过。
   这仍没有IRQ allowlist，启停接口仍依赖调用者只操作自己的IRQ；Linux在suspend
   恢复中也存在整组SPI寄存器恢复。当前禁止跨OS独立suspend/restart，不能以
   初始化及配置冻结测试代替完整隔离。
3. **串口归属缺口**：现有AMP展开配置实见`CONFIG_NO_SERIAL_CONSOLE=y`、
   `# CONFIG_UART0_SERIAL_CONSOLE is not set`，此前编译不能证明NSH串口可用。
   应将NuttX console启用、Linux console排除和UART0 IRQ108归属作为独立改动验证。
4. **IRQ路由**：现有BB3/IRQ174仍路由小核3；必须结合四核实际起核次序与最终DTB
   检查，不以Linux逻辑CPU号代替物理MPIDR/target mask。
5. **RPTUN**：本地resource table的DRIVER_OK仍无可靠发布来源；需真实主机握手，
   不能通过无条件置位绕过。共享缓存、早到notify及重启generation另测。

后续顺序：完成上述启动前门禁 → 非启动大核Linux交接 → 最小RPMsg health →
故障注入与恢复 → 最后接入AMP A/B。普通NuttX A/B保留为恢复基线。

## 冷启动握手：实现前必须满足的顺序

本节是源码审查得出的约束，握手代码尚未接入N-Boot，不能作为已实现功能。

Rockchip原版`u-boot/drivers/cpu/rockchip_amp.c:setup_sync_bits_for_linux()`在
启动非boot CPU上的Linux之前清GICD_CTLR使能位和末尾优先级word；Linux的
`irq-gic-common.c:gic_dist_config()`重写优先级，`irq-gic.c:gic_dist_init()`最后
使能distributor。因此可以复用这个协议，但必须同时保证本轮fresh初始化。

1. **先验证再写入**：完整FIT长度、payload边界/哈希、八核归属、DDR范围和
   预期IRQ路由通过后，才允许bootm FINDOTHER向目标地址装载。
2. **保护仍在运行的N-Boot**：最新N-Boot的`lib/lmb.c:lmb_reserve_uboot_region()`
   保护栈底`gd->start_addr_sp - CONFIG_STACK_SIZE`至原始relocation上界等区域，
   但LMB只是N-Boot分配器的账本，不自动成为Linux内存保留契约。最终Linux DTB
   必须保留等待交接期间仍使用的N-Boot代码、栈、堆及相关数据；不能启动Linux
   后仍在Linux可以回收的内存里轮询。需处理SKIP_RELOC的额外代码区并逐区验证。
3. **先停止旧DMA/中断使用者**，再清GIC同步标记。只允许没有另一OS仍运行的
   冷启动流程；不能用这个清零步骤尝试恢复已运行的Linux。
4. **清除旧状态并回读确认**：与当前Linux实际初始化范围一致地选择有效SPI
   尾部标记，注意Linux对最大IRQ数量的裁剪；不得把保留ID当标记。标记、使能
   与预期路由检查必须属于本次清零之后，旧的GICD_CTLR非零不能通过。
5. **传递最终Linux入口和DTB**：复用标准bootm结果；向大核0x100交接时x0为
   最终DTB、x1..x3满足ARM64启动约定。PSCI CPU_ON成功仅证明启动请求受理。
6. **有限等待**：验证标记更新、distributor启用和私有IRQ路由；有明确超时，
   不直接无限WFE。成功后才让小核0进入启用PREINITIALIZED/STATIC_SPI的NuttX。
7. **起核后的失败不能普通回退**：一旦Linux CPU_ON成功，即使等待超时也可能
   有Linux继续运行。不能直接进入会重置GIC的普通bootnuttx；必须整机重启，或
   先使用已经验证的固件接口停止计算域。AMP A/B接入后，尝试次数需在起核前落盘。

以上检查只覆盖冷启动。独立Linux重启、共享父时钟、GPIO组中断和内核访问隔离
另有验收条件；目前不承诺支持。
