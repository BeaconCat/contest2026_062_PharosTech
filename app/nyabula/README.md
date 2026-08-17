# Nyabula openvela APP

Nyabula 的产品代码从模拟器阶段起就在 openvela/NuttX 中运行。当前首个功能节点使用 LVGL 9，在一个 `720×360` 的 sim framebuffer 中模拟左右两块 `360×360` 圆屏。

## 当前边界

- 左、右是猫自身的物理语义，不随宿主窗口或板级走线变化。
- `eye rig` 统一生成双眼状态，避免维护两套 UI。
- 动画按 LVGL 单调 tick 采样，掉帧不会改变动画速度。
- X11 等宿主窗口实现仅是 `sim` 板的显示后端，产品代码不调用桌面 API。
- 当前 sim renderer 在一个 LVGL display 上使用双视口；真实屏幕适配层可将同一逻辑帧
  映射到两个独立的 `360×360` buffer 和 flush queue，不改变 Eye Engine 或 Core。

## 构建基线

先通过团队 manifest 同步工程，让本目录链接到：

```text
packages/demos/contest2026_062_nyabula
```

以 NuttX `sim:lvgl_fb` 为基线配置，再合入 `configs/sim.config`。宿主 framebuffer 尺寸为 `720×360`，Nyabula 注册为 NSH builtin APP，运行命令为：

```text
nyabula
```

运行产物后应看到两块并列圆形猫眼，双眼同步凝视、呼吸并周期性眨眼。代码仅依赖 openvela/NuttX 和 LVGL，不依赖宿主 SDL API。

默认启动待机眼睛：

```text
nyabula
```

自动轮播首批语义表情：

```text
nyabula demo
```

自动轮播 Web Demo 已定义的全部 eye scene：

```text
nyabula scenes
```

场景不是硬编码的展示页。`nyabula_eye_engine_show_scene()` 接收固定 schema 的
typed payload；媒体进度、倒计时、电量、天气、来电、任务和系统状态可在场景
显示期间由 Nyabula Core 调用 `nyabula_eye_engine_update_scene()` 热更新。完整字段
见 `SCENE_SCHEMA.md`。

## 代码分层

- `nyabula_eye_engine.c`：语义表情、贝塞尔过渡、凝视和眨眼时间线；
- `nyabula_eye_renderer_lvgl.c`：LVGL 对象与左右 eye surface；
- `nyabula_eye_engine.h`：后续 Nyabula Core 使用的稳定控制 API；
- `generated/`：从批准的 SVG 与字体源机械生成的只读固件资源；
- `nyabula_main.c`：OpenVela APP 生命周期和模拟器 showcase。

整体控制系统、后端和多端路线见 `ARCHITECTURE.md`。
真实双屏对接边界、左右语义和当前 buffer 布局见 `DISPLAY_INTEGRATION.md`。
