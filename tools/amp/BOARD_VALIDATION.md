# 2026-09-10 最小AMP板测

KICKPI-K7，4GiB DDR，SD启动，CH340 COM11@1500000。串口以实际枚举为准。
eMMC未写，普通NuttX A/B保留，AMP FIT仅下载到RAM。

- N-Boot FIT SHA256：5aa532b8d4f6dea08068dd172f1e47f874deb4c9466b55bddbc64923acdd4930。
- AMP FIT：13587968字节，SHA256 83e653ec04a4cd520efd011b7cd203de1d39f782953b8e5103aa26a4db6a7dcd。
- 启动命令：bootamp 60000000 cf5600。

## 实测确认

Linux日志确认MPIDR 0x100..0x103启动并执行/init；openvela进入NSH，ps显示
CPU0..3各有IDLE线程。真实RPMsg输出（不是PTY/QEMU）：

```text
nyamp health ok: generation=54195491 capabilities=0x00000001
nyamp Linux info: generation=54195491
online=4
cpu0 part=0xd08
cpu1 part=0xd08
cpu2 part=0xd08
cpu3 part=0xd08
```

重复health保持同一generation。

## 本轮修复

1. N-Boot PSCI未probe，psci_method=0导致查询返回disabled。入口先probe。
2. MAILBOX3 A2B未使能，cmd/data已写而status为0。启动器预使能接收，
   保留首个kick给openvela，CPU0 IRQ仍关闭。
3. openvela本地资源表缺CPUNAME配置，触发rpmsg_virtio.c:835断言。
   补齐本地元数据，不要求Linux支持该私有资源表。
4. Linux不会回发动态端点公告。nyampd先发HEALTH/READY事件，OpenAMP从首帧
   学习对端地址；客户端等待绑定并跳过该事件，随后执行请求/应答。

## 未验证与已知限制

本节描述最初RAM候选；后续SD槽启动结果见SLOT_BOOT.md，NPU结果见下节。
长稳、独立重启、完整模型推理、产品外设共存仍未验证。
Linux仍有SDK其他外设日志；bootm对已保留区域打印重复LMB reservation警告，
本轮未影响启动，不能将此候选称为完整产品发行版。

N-Boot等待超时会复位整个SoC回普通NuttX；openvela断言可能需要物理Reset。
普通NSH可用nbootctl reboot console再次进入启动器；最小AMP无ADB。

## 本地NPU候选收尾（2026-09-10）

- FIT：17681920字节（16.8628MiB），SHA256
  `acd450e6224a85aa55cc829fac232a1cffd48821a8a27f36ec4081aab4e84a37`。
- Linux内核和DTB与既有4+4 AMP基线一致；新增外部RKNN v2.3.2用户态库。
- SD AMP A version6，AMP B version1和普通NuttX A/B保留，未写eMMC。
- 固定INT8矩阵乘M1/K64/N32，openvela独立校验全部32项，无CPU后端回退。
- 本次收尾连续20次请求通过；软件复位、回普通NuttXverify/rearm后，再20次通过。
  种子序列为`1 2 0 255 7 31 64 127 254 1`重复两遍。
  两轮health generation分别为1872780287、1310416904，capabilities均为3。
- 用户确认主电源断开再接回，11:37:07自动加载A version6 (trial)并进入NSH；
  随后同一序列20次NPU请求全部通过，health generation=3546581191，capabilities=3。
  本次收尾共60次请求通过；原始冷启动记录为
  `串口日志/console_20260910_113638.log`，不是用软件复位代替冷启动。
- 验证后在普通NuttX中verify/mark-successful AMP A成功，bootctrl generation115，
  A successful=1、B successful=1。再重启输出`bootamp: loaded SD slot a, version 6 (confirmed)`；
  额外一次NPU计算通过，info返回四个A72，health generation=3114657656、capabilities=3。
- 以上是有限次验收，不是长时间压力测试；NPU公开镜像还需完成第三方许可核验。

实测有效组合：使用SDK默认同步，不对C输出内存预先memset。
去掉手动输入同步单独不足以修复；先前出现全零/半段错误，原始失败保留在
项目实测日志。CPU脏缓存行影响DMA是机制推断，未宣称已证明SDK内部根因。
