# AMP FIT 与 N-Boot 边界

`build_amp_fit.sh` 从全新 ITS 生成 external-data FIT，同时装入 Linux Image、K7
DTB、initramfs 和 openvela CPU3 固件。它不修改已有 vendor FIT，避免历史上因
`fdtput` 重排 external data 导致其他 payload 被截断或 hash 失效。

固定加载地址：

| payload | load/entry |
|---|---:|
| Linux Image | `0x42000000` |
| K7 DTB | `0x4f000000` |
| initramfs | `0x50000000` |
| openvela | `0x4a400000` |

脚本验证 Linux Image/DTB magic、gzip、openvela 16 MiB 上限、FIT load/entry 和每段
SHA-256。FIT 的 `loadables` 只负责把 openvela 复制到保留区；N-Boot 仍须在跳转
Linux 前调用RK3576 AMP SIP，以`cpu_id=3, entry=0x4a400000`启动控制域。该CPU_ON
路径必须上板确认，不能用“FIT打包成功”代替。

```sh
tools/amp/nboot/build_amp_fit.sh \
  out/linux/arch/arm64/boot/Image \
  out/linux/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-nyabula-amp.dtb \
  out/nyabula-amp-initramfs.cpio.gz \
  out/openvela/nuttx.bin \
  out/nyabula-amp.itb
```

首轮可把同一FIT写入`amp_a`，`amp_b`保持禁用。确认CPU/DDR/IRQ/RPMsg和故障恢复
后，再扩展现有N-Boot bootctrl格式管理`amp_a/amp_b`，不要在尚未板测时复制一套
未经验证的A/B状态机。
