# AMP 对接最新团队仓与 N-Boot

## 2026-09-07 基线迁移

- 团队基线：`59e09ed5`，含 PR #76/#79/#78。
- 新工作树：`tmp/k7-amp-integration-20260907`。
- 新分支：`work/k7-amp-integration-20260907`。
- N-Boot 基线：`5ef4155fc4dc841e32530cb3d6a578a7743fe791`。
- N-Boot 新工作树：`tmp/nboot-amp-integration-20260907`。
- N-Boot 新分支：`work/amp-integration-20260907`；当前仅建分支，未加入启动代码。
- 原 `tmp/k7-amp-runtime`、`tmp/nboot-amp` 原样保留，不修改旧历史。

仅重放 `b343d272..54297fa5` 的14个AMP专属提交，不重放已被团队以不同历史
合入的WiFi改动。解决冲突时保留团队RTC/WiFi初始化、timer/SARADC构建入口及
寄存器定义。已有4+4未提交修改随后恢复到新工作树，测试脚本仍仅本地保留。

链接脚本合并保留 `.bss/.initstack NOLOAD` 和 `_szdata` 的运行范围，同时恢复
`CONFIG_RAM_START`；另加RAM末端ASSERT。新分支相对团队基线为0个merge提交。
对照确认主线WiFi/RTC/时钟/eMMC/timer/TSADC/SARADC实现、drivers、N-Boot资产
与`tools/k7_pack`没有被迁移覆盖。

## 验证结果（编译通过，未上板）

构建目录：`/root/openvela/amp-integration-20260907`。独立复制现有repo工程中的
nuttx/apps/external，重定向构建用绝对软链；工具链复用原安装，不修改原Core构建树。
团队输入为新分支归档及未提交差异；Linux副本的文本统一LF，二进制资产不转换。

| 配置 | 构建 | 入口 | bin字节 | 运行时末端 |
|---|---|---|---:|---|
| AMP四A53 | Make | 0x4a400000 | 314704 | 0x4a477000 |
| AMP四A53 | CMake | 0x4a400000 | 314744 | 0x4a477000 |
| 普通NSH | CMake | 0x40200000 | 236400 | 本轮未单独记录 |

AMP两份展开配置通过校验，13组拓扑/FIT主机测试通过。ELF确认BSS和initstack
是NOBITS，未把文件缩小误当成运行内存缩小。Make/CMake日志位于服务器
`/tmp/amp-integration-{make,cmake,nsh}.log`。

## 下一阶段边界

2026-09-07继续：GIC初始化不破坏共享寄存器的内核选项及离线验证已完成，
但产品配置尚未启用，握手和运行期隔离未完成；详见[OWNERSHIP.md](OWNERSHIP.md)。

1. 固定内存/IRQ/时钟/电源/外设所有权，修复NuttX共享GIC初始化路径。
2. 在最新N-Boot实现4+4交接：准备最终Linux DTB，在大核0x100启动Linux，
   当前小核0进入NuttX。旧CPU3命令不移植、不作为兼容路径。
3. 先完成最小双系统与RPMsg health，再扩展AMP A/B。
4. 复用现有两个512MiB AMP槽与bootctrl第二domain；不重写分区布局，不把AMP
   塞进4MiB N-Boot FIT。handoff v2还没有domain，必须设计兼容扩展及成功确认。

本轮没有完成跨簇启动、GIC隔离或RPTUN握手；没有推送、没有生成可刷发布镜像。
