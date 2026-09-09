# nyampd

`nyampd` 是 Linux AMP 计算域唯一的控制入口。它直接读写由
`rockchip_rpmsg_mbox` 和 `rpmsg_char` 创建的消息型字符设备；默认从sysfs寻找名字为
`rpmsg-raw`的endpoint对应的`/dev/rpmsgN`，不依赖动态编号。收到畸形包时丢弃，
transport EOF/错误或短写时立即退出，让 PID1 重新拉起并生成新的 generation。

当前只实现 `health.query`，这是有意的停止边界：NPU、ISP、ASR/TTS 服务必须在各自
vendor runtime 和 buffer 生命周期确定后作为独立提交加入，不能先返回伪成功。

deadline 使用两端共享的 ARM generic counter 换算毫秒。AArch64 生产构建读取
`cntvct_el0/cntfrq_el0`；主机单测显式注入当前值，不依赖主机时钟。

主机构建与测试：

```sh
cmake -S tools/amp/nyampd -B out/nyampd
cmake --build out/nyampd
ctest --test-dir out/nyampd --output-on-failure
```

运行：

```sh
/usr/sbin/nyampd
```

也可在诊断时显式传入设备路径。openvela侧`nyampctl health`首次运行会创建并公告
`rpmsg-raw` channel，initramfs supervisor会在endpoint出现后自动拉起daemon。
