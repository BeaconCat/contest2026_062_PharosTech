# KICKPI-K7 AMP 最小上板收敛清单

目标拓扑：四A53运行NuttX、四A72运行Linux。当前启动交接、GIC隔离、RPTUN握手
尚未完成离线修复；本清单不是当前板刷许可。每轮固定使用 COM14、K7 Monitor 穿重启抓串口；
同时可直接使用ADB采集Linux日志，不另开可见窗口。任何“预期”都不能勾成成功，
必须保留原始输出并写入`实测日志.md`。

## 0. 上板护栏

- 从已知可恢复的R2/N-Boot卡启动，不覆盖当前健康槽；首轮只把AMP FIT放`amp_a`，
  `amp_b`保持禁用。
- 校验FIT SHA-256为离线记录值后再加载；禁止手术修改external-data FIT。
- 首轮Linux RAM-only，不初始化MMC/eMMC/SD/USB存储驱动；不得写eMMC。
- N-Boot交接前必须确认Linux DTB已禁用全部四个A53并启用全部四个A72。
  Linux主核为物理MPIDR `0x100`，NuttX主核为`0`；禁止对当前运行核调用CPU_ON。
- 任何连续重启、串口完全消失、loader/maskrom或存储枚举异常都先回已知好镜像，
  不连续盲刷。

## 1. N-Boot与内存隔离

验收证据：

1. `dumpimage -l`四段hash、load和entry全部通过；
2. Linux启动参数中的DTB就是AMP DTB；
3. Linux只online四个A72，物理MPIDR为`0x100..0x103`；NuttX四核均为A53，
   物理MPIDR为`0..3`。不能仅凭两边的逻辑CPU0..3或online数量判断；
4. `/proc/iomem`或reserved-memory调试输出包含：
   - `0x47800000..0x47a00000` vring；
   - `0x47a00000..0x47c00000` rpmsg DMA；
   - `0x47c00000..0x48000000` service shmem；
   - `0x4a400000..0x4b400000` openvela；
5. OP-TEE仍为`0x48400000..0x49400000`，与openvela无交集；
6. N-Boot/BL31启动Linux主核`0x100`，传入最终DTB地址，然后当前小核进入
   `0x4a400000`的NuttX；两个OS各自成功启动同簇另外三个核；
7. NuttX初始化和启动次核期间，Linux的SPI路由、使能和优先级没有被全局重置。

失败即停：CPU双重online、reserved-memory缺失、OP-TEE异常、SMC不支持或入口不符。

## 2. Mailbox与RPMsg建链

Linux侧检查：

```sh
dmesg | grep -Ei 'rockchip.*(amp|mailbox|rpmsg)|virtio|rpmsg'
find /sys/bus/rpmsg /sys/class/rpmsg -maxdepth 3 -type f -print
ls -l /dev/rpmsg*
```

openvela侧检查：

```text
ls /dev/rpmsg
nyampctl health
```

必须看到：

- Linux `rockchip_amp_probe`、mailbox0/3和`rockchip_rpmsg_probe`成功；
- openvela存在`/dev/rpmsg/linux`；
- `nyampctl health`公告`rpmsg-raw`，Linux生成对应`/dev/rpmsgN`；
- `nyampd`自动发现endpoint并返回非零generation；
- openvela打印`nyamp health ok`，request id、generation和capability一致；
- mailbox3 remote接收中断确认为BB3/IRQ174，而非AP3/IRQ160。

## 3. 缓存与持续流量

在health通过后增加专用压力命令，至少覆盖：

- 0、1、63、64、455、456-byte inline payload；457-byte必须本地拒绝；
- 双向各10000次请求，request id单调且无丢失/重复；
- payload起始和长度覆盖cache-line非对齐组合；
- mailbox busy时不破坏已入队vring，后续kick能让对端扫到全部消息；
- Linux TX buffer消费通知走mailbox3，长跑时vq1不会耗尽；
- 记录吞吐、P50/P95/P99、最大连续无错误次数及两端CPU占用。

只有压力通过后，`CONFIG_OPENAMP_CACHE`和Linux coherent DMA组合才能标为实测确认。

## 4. 故障恢复

- `kill -9 nyampd`：PID1在1秒级重启，generation改变，openvela丢弃旧响应；
- 在请求进行中杀daemon：请求在deadline结束，不能永久阻塞；
- 发送坏magic/version/flags/长度：daemon丢弃或返回protocol error，不崩溃；
- 重启Linux计算域：openvela表情、WiFi、蓝牙和基础交互持续运行；
- Linux恢复后重新发现channel，health自动恢复；
- 连续3次Linux启动失败：停止自动重试并进入明确降级态，不能拖入整机重启环；
- openvela panic/reset不能留下Linux继续写旧generation共享buffer。

## 5. NPU/ISP服务接入顺序

transport稳定前不接vendor runtime。之后每项独立提交、独立验收：

1. `npu.load`：固定小模型、版本/hash、CMA分配和卸载；
2. `npu.infer`：共享buffer descriptor、输入输出shape、deadline/cancel；
3. `vision.track`：IMX415→CIF→ISP v39→RGA/MPP最小帧链；
4. `asr.transcribe`、`tts.synthesize`：音频仍由openvela拥有，Linux只处理共享帧；
5. Linux服务崩溃分别验证，不得让设备控制权或用户状态迁入Linux。

## 6. 结束条件

以下全部满足才结束AMP bring-up：

- CPU、DDR、OP-TEE、IRQ和外设所有权无冲突；
- Make/CMake、Linux Image、K7 DTB、initramfs、FIT与主机测试继续全绿；
- RPMsg双向建链、缓存压力、daemon/Linux重启和旧generation隔离实测通过；
- 至少一个固定NPU模型和一条IMX415最小视觉链实测通过；
- 所有串口、dmesg、命令与hash按日期进入实测日志；
- 未验证项仍明确标“编译通过”或“仅推断”，不包装成完成。
