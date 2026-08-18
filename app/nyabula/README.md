# Nyabula openvela APP

Windows 本地 Core/Service/Mock Eye 开发入口见
[`host/README.md`](host/README.md)。原始 HTML 视觉 Demo 保持为金标准，Windows Mock 是独立复制文件。

Nyabula 的产品代码从模拟器阶段起就在 openvela/NuttX 中运行。Eye Engine 使用 LVGL 9 Vector Graphic API 与 ThorVG 抗锯齿软件后端，并始终渲染到左、右两个独立 `360×360` surface。sim 的 `720×360` framebuffer 只负责并排展示这两个 surface，不属于 Eye Engine 的内部画布。

## 当前边界

- 左、右是猫自身的物理语义，不随宿主窗口或板级走线变化。
- `eye rig` 统一生成双眼状态，避免维护两套 UI。
- 动画按 LVGL 单调 tick 采样，掉帧不会改变动画速度。
- X11 等宿主窗口实现仅是 `sim` 板的显示后端，产品代码不调用桌面 API。
- renderer 从创建阶段就持有两个独立 `360×360` 双缓冲 surface；真实双屏通过
  `nyabula_eye_engine_create_dual()` 分别传入左右 display root，sim 则把两个 canvas
  并排挂到同一个 root。两种模式不发生逐帧拆分或 framebuffer 复制。

## 构建基线

先通过团队 manifest 同步工程，让本目录链接到：

```text
packages/demos/contest2026_062_nyabula
```

以 NuttX `sim:lvgl_fb` 为基线配置，再合入 `configs/sim.config`。宿主 framebuffer 尺寸为 `720×360`，Nyabula 注册为 NSH builtin APP，运行命令为：

```text
nyabula
```

当前 openvela 基线的 NuttX libc++ 与宿主 GCC 14 存在 builtin trait 兼容问题；使用
GCC 14 构建 sim 时追加 libc++ 已提供的兼容宏：

```text
make -C nuttx -j4 EXTRAFLAGS=-D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS
```

完整字体位于 `res/fonts`。sim 可用 HostFS 把 `res` 挂到 `/data/nyabula` 后启动：

```text
mkdir /data
mkdir /data/nyabula
mount -t hostfs -o fs=/absolute/path/to/app/nyabula/res /data/nyabula
nyabula demo
```

未挂载资源文件系统时会使用编译内置字体作为启动安全 fallback，但这不能替代正式
视觉回归；真机应把完整字体部署到 `CONFIG_CONTEST2026_062_NYABULA_FONT_ROOT`。

运行产物后应看到两块并列圆形猫眼，双眼同步凝视、呼吸并周期性眨眼。代码仅依赖 openvela/NuttX 和 LVGL，不依赖宿主 SDL API。

默认启动 Core，并以最低优先级每 5 秒轮播首批语义表情，方便在网络驱动尚未接通时直接上板展示：

```text
nyabula
```

轮播使用 `startup-showcase` 状态源；任何真实表情控制都会暂停轮播，控制租约释放后自动恢复，Scene 显示不会被覆盖。显式启动同一展示模式：

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
