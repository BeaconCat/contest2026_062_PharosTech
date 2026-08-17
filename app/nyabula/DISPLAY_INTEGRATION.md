# Nyabula 双屏显示对接

## 当前模型

Eye Engine 每个 tick 产生固定顺序的两份逻辑帧：

```text
frames[NYABULA_EYE_LEFT]  -> 猫的左眼
frames[NYABULA_EYE_RIGHT] -> 猫的右眼
```

左右是产品物理语义，不是 framebuffer 的临时编号。板级走线若交换了屏幕片选，
应在显示适配层修正映射，不能交换 Eye Engine 的左右语义。

当前 sim renderer 为了在 X11 单窗口中观察双眼，使用一个 `720×360` canvas：

```text
x =   0..359  -> 左眼 360×360 视口
x = 360..719  -> 右眼 360×360 视口
```

renderer 内的 `buffer[0]` 和 `buffer[1]` 是相邻帧的时间双缓冲，尺寸均为
`720×360`；它们不是左屏和右屏 buffer。

## 与真实双屏实现的边界

队友的两个独立 `360×360` buffer 属于显示适配层。推荐保持以下职责边界：

- Eye Engine：表情、凝视、眨眼、Scene、转场和单调时间线；
- LVGL renderer：把两份逻辑帧分别栅格化为左右 eye surface；
- board/display：buffer 生命周期、cache flush、脏区、FSPI 排队和屏幕片选；
- Nyabula Core：来源、优先级、lease 和状态真源，不接触 framebuffer。

短期联调可以在现有 `720×360` 帧完成后按行拆成两个 `360×360` buffer，以验证
物理左右、颜色格式和 flush。正式实现应让 renderer 使用两个本地坐标均为
`0..359` 的 surface，避免每帧拆分和复制；原本位于 `x=180/540` 的眼睛中心在
两个 surface 中都应是 `(180, 180)`。

两块屏即使共享 FSPI、必须串行传输，也应保留独立的 flush completion。动画时间
继续从同一个 LVGL 单调 tick 采样，不能按某块屏的传输完成次数推进，否则总线
拥塞时左右眼会逐渐失步。

## 建议的合并顺序

1. 先让板级双屏驱动独立显示纯色和固定测试图，确认左右与色序；
2. 用现有合成帧按行拆分，验证 Eye Engine 在真实屏幕上的几何和动画；
3. 将 renderer 的输出目标重构为两个独立 surface，消除拆分复制；
4. 最后接入脏区和共享 FSPI flush queue，测量稳定帧率与最坏传输延迟。

双屏驱动不应复制表情状态机或 Scene 绘制代码；Eye Engine 也不应包含 ST77916、
FSPI、片选或 DMA 细节。这样双方分支只会在 renderer 创建接口和 APP 启动代码
发生小范围对接，不会在产品逻辑上互相覆盖。
