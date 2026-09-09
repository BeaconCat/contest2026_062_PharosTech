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

压力/长稳、独立重启、NPU推理、产品外设共存、AMP槽自动加载未验证。
Linux仍有SDK其他外设日志；bootm对已保留区域打印重复LMB reservation警告，
本轮未影响启动，不能将此候选称为完整产品发行版。

N-Boot等待超时会复位整个SoC回普通NuttX；openvela断言可能需要物理Reset。
普通NSH可用nbootctl reboot console再次进入启动器；最小AMP无ADB。
