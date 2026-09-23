# AMP活动槽启动与回退（本地验证版）

本地分支：团队`feat/amp-slot-activation`，N-Boot `feat/amp-slot-boot`。
本阶段没有推送或变更PR。普通NuttX A/B保留，测试仅写SD，不写eMMC。

## 启动策略

full版N-Boot按顺序尝试活动AMP槽、普通NuttX、Fastboot。只看bootctrl中
AMP domain的active_slot，不自动轮换到另一个AMP槽，也不新增记录格式。
槽载入区是0x60000000，当前自动加载上限64MiB。

| 活动AMP槽状态 | 行为 |
|---|---|
| priority=0或image_size=0 | 跳过，启动普通NuttX |
| successful=0且tries=0 | 跳过，启动普通NuttX |
| successful=0且tries>0 | 先落盘减一次，再加载、校验、启动 |
| successful=1 | 校验后启动，不消耗试次 |
| 读取或校验失败 | 返回普通NuttX启动路径 |

新nbootctl的stage amp和未确认槽set-active amp会设置一次试启动。
NuttX domain原有零试次启动策略不变。一次性reboot nuttx-a/b优先于自动AMP。

## 安装与确认

先在普通NuttX中安装带新nbootctl的固件及配套完整N-Boot FIT，保留已知好备份。
不能把裸N-Boot proper写入启动槽，也不能把AMP FIT写入nuttx_a/b。

下面仅以stage实际输出选择b槽为例；若输出a，后续命令相应改为a。

```text
nbootctl stage amp /tmp/amp.itb
nbootctl status
reboot
```

AMP NSH中执行：

```text
nyampctl health
nyampctl info
reboot
```

未确认的试次已消耗，重启应回普通NuttX。确认刚刚实测通过的镜像：

```text
nbootctl verify amp b
nbootctl mark-successful amp b
reboot
```

此后正常重启自动进入已确认AMP。AMP最小配置没有SD host或nbootctl，因此
成功确认在普通NuttX执行，不让两内核同时写盘。

## 恢复与边界

普通NuttX可用nbootctl reboot nuttx-a/b请求一次普通启动；AMP中可在重启时
通过现有串口持续发送!进入N-Boot，再执行bootnuttx。物理Recovery/Fastboot入口保留。

successful是操作员确认，不是持续健康监测。已确认内核后续崩溃仍需上述恢复
入口，本阶段不包含看门狗降级、完整AMP A/B策略或产品外设集成。
可选NPU镜像提供固定矩阵乘测试，不包含完整模型服务。

## 已完成验证

- 空AMP槽自动回普通NuttX。
- SD AMP B槽直接试启动，真实health/info返回四个A72。
- 未确认就复位，tries由1变0，自动回普通NuttX。
- 内部哈希损坏的FIT被拒绝，同一次启动立即回普通NuttX。
- 验证后mark-successful，正常重启以confirmed启动。
- 指定nuttx-a的一次性请求覆盖AMP自动启动，下一次正常重启恢复AMP。
- 损坏测试镜像已由验证过的好镜像覆盖。

2026-09-10用户执行断电再上电后，串口记录09:58:47自动加载SD槽b version1
(confirmed)，09:58:48进入NSH；health/info的generation=1140570433，online=4，
四个CPU part均为0xd08，重复health正常。原始启动记录为工作区
串口日志/console_20260910_094355.log。本次有用户物理操作确认，不是仅凭软件
复位日志中的cold boot字样判定。
