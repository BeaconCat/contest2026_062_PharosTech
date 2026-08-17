# Nyabula 产品运行时架构

## 1. 目标

Nyabula 的双眼不是一段由云端播放的视频，而是设备本地持续运行的实时界面。
手机、语音助手、LLM 和传感器只提交语义意图，设备端负责仲裁、动画和渲染。
这样即使网络断开或模型响应变慢，凝视、眨眼和状态反馈仍然连续自然。

HTML 猫眼 Demo 是视觉规格与算法原型，不是最终运行时。浏览器提供的 DOM、
Canvas 2D、`Path2D` 和 `requestAnimationFrame` 在 openvela QuickJS 中不存在。
其中的表情参数、贝塞尔时间线和粒子算法应移植到本地 Eye Engine，绘制由 LVGL
后端完成。

产品功能分级、双屏信息设计和实施路线见
`工具/Nyabula产品功能与三路线设计.md`。

### 1.1 AMP 架构红线

openvela/NuttX 是产品主体，必须持有 Nyabula Core、AI Agent 主循环、人格与
记忆、工具权限、双屏 UI、媒体会话、设备状态、定时任务和连接生命周期。
Linux AMP 仅用于 NuttX 暂时无法完成的 NPU 推理、ASR/TTS、摄像头 ISP、视觉
算法和重型编解码，并通过有超时、取消和版本协商的计算 RPC 返回结果。

Linux 不能决定表情、媒体队列或工具执行，不能成为产品后端和状态真源。杀死
Linux 计算服务后，猫眼、媒体控制、闹钟/计时器、基础交互和设备控制必须继续
运行；openvela 应降级到规则、云模型或轻量本地能力，并可独立恢复计算服务。

## 2. 分层

```text
Flutter / Web / BLE / 语音 / 传感器 / LLM Skill
                         │
                         ▼
              Nyabula Control Gateway
 MCP Client/Server、OpenClaw Node、WebSocket、BLE、云端 WSS
                         │
                         ▼
                   Nyabula Core
       状态机、来源优先级、会话、记忆、AI 调度
           ┌─────────────┼──────────────────────┐
           ▼             ▼                      ▼
 openvela ai_agent   semantic eye intent   AMP Compute RPC
 Skills/工具/定时          │              NPU/ISP/ASR/TTS only
 LLM 路由/MCP              ▼
                    LVGL Eye Engine
       表情参数、贝塞尔时间线、自动眨眼、凝视
                         │
                         ▼
                LVGL Eye Renderer
             左眼 surface / 右眼 surface
                         │
          ┌──────────────┴──────────────┐
          ▼                             ▼
 openvela sim framebuffer       ST77916 ×2 / QSPI
```

## 3. Eye Engine 的职责

Eye Engine 必须在设备本地运行，并使用单调时钟推进动画：

- 接收 `idle`、`curious`、`processing`、`sleep` 等语义表情；
- 管理当前参数、目标参数和可打断的贝塞尔过渡；
- 管理自动眨眼、显式眨眼、凝视和待机微动；
- 管理 `music`、`timer`、`weather`、`battery`、`call`、`task` 和 `eq` 等预定义
  eye scene，不允许外部提交任意控件树；
- 额外 UI 必须从当前眼皮状态执行“闭眼换景→睁眼显露→闭眼清除”转场；
- scene 图形、粒子、数字和文本必须与睡眠 Zzz 一样受上下眼皮几何并集蒙版
  裁剪，眼皮重合不能互相抵消；
- 保证左、右眼的物理语义固定，不受板级接线影响；
- 掉帧只降低视觉帧率，不改变动画速度；
- 将一帧确定的参数交给 renderer，不处理网络和 LLM。

后端不得逐帧发送 `lid_top=0.73`，也不得直接创建 LVGL 信息卡。后端只允许
请求语义动作或预定义 eye scene，例如：

```json
{
  "action": "eyes.play",
  "expression": "curious",
  "intensity": 0.8,
  "duration_ms": 1500,
  "transition_ms": 280,
  "priority": 40,
  "source": "assistant"
}
```

```json
{
  "action": "eyes.scene.show",
  "scene": "weather",
  "target": "right",
  "payload": { "condition": "rain", "temperature": 23 },
  "transition": "close_reveal",
  "lease_ms": 8000
}
```

## 4. Nyabula Core 的职责

Nyabula Core 是后续节点，建议使用 C++17；LVGL renderer 和硬件接口保持 C。
Core 负责把不同来源转换成统一 intent，并按优先级仲裁：

1. 电源、温度和故障等安全状态；
2. 触摸、唤醒、插拔等物理反射；
3. 当前语音对话反馈；
4. 用户从手机发出的显式控制；
5. LLM/Claw 生成的情绪和动作；
6. 自动眨眼、呼吸和待机微动。

高优先级动作获得带期限的 lease。动作结束或 lease 超时后恢复低优先级状态，
而不是由多个来源同时修改 UI 参数。

LLM 只能调用结构化工具，例如 `eyes.play`、`eyes.blink`、`eyes.gaze`，不能直接
访问 renderer。模型输出必须经过 schema 校验、限幅和 Core 仲裁。

电脑 Agent 通过局域网 Streamable HTTP 连接 openvela MCP Server 时，也必须
经过同一 Tool Gateway 和 Core 仲裁。当前 `ai_agent` MCP Server 只有 stdio
transport；产品需要补齐网络 transport、初始化握手、配对 token、scope、限流
和审计。Linux AMP 不监听 MCP 端口，也不直接接收电脑 Agent 的设备控制请求。

## 5. 多端与语言

| 部分 | 推荐实现 |
|---|---|
| LVGL Eye Engine / renderer | C |
| Nyabula Core | C++17 |
| 可选 Skill 脚本 | QuickJS，不能进入逐帧渲染路径 |
| 手机 | Flutter / Dart |
| 内置 Web 控制台 | TypeScript |
| 公网同步服务 | Go |
| 局域网控制 | WebSocket + JSON v1 |
| 离线控制 | BLE GATT，后续可使用紧凑二进制载荷 |
| 远程连接 | 设备主动连接 Go 服务的 WSS |

电话和媒体控制由 openvela Fluoride 蓝牙栈产生事件，再交给 Nyabula Core；不应
让 Flutter 或 LLM 越过 Core 直接操作蓝牙和 UI。

## 6. 双屏抽象

逻辑层与 renderer 始终持有左、右两个独立 `360×360 eye surface`。模拟器可在一个
`720×360` 宿主 framebuffer 中把两个 canvas 并排展示，但不允许创建合成 draw
buffer 后再拆分。真实双屏通过双 parent API 直接绑定各自的 display root。

硬件阶段注册两个独立 display/flush queue，分别绑定 ST77916 CS。两块屏共享
FSPI 总线，因此 flush 可独立排队，但最终由总线串行执行。Eye Engine 不感知
framebuffer、片选或左右声道式的板级映射。

逻辑更新建议保持 60 Hz；受 QSPI 带宽限制，硬件渲染/flush 可以是 30～45 Hz，
所有动画仍按单调时间采样。

## 7. 实施节点

### M1：LVGL Eye Engine

- [x] OpenVela APP、Kconfig/Make/CMake 收录；
- [x] 两个独立 `360×360` surface 在 `720×360` sim 宿主中并排编译；
- [x] 模型、时间线、renderer 分层；
- [x] 语义表情 API、显式眨眼和凝视 API；
- [x] 从 HTML 迁移全部表情与粒子；
- [x] renderer 双 surface 与双 display root 创建 API；
- [ ] 板级两个独立 LVGL display/flush queue；
- [x] 预定义 eye scene、typed payload 与闭眼换景协议；
- [x] 所有 scene 共用上下眼皮并集剪贴蒙版；
- [x] sim 窗口运行；
- [x] sim 视觉回归（ThorVG 实际运行，完整字体及待机/音乐/计时器截图通过）。

### M2：Nyabula Core

- [x] 固定容量 intent 队列、来源优先级、lease 和到期恢复；
- [x] expression/scene 独立租约槽，scene hide 释放本来源所有权；
- 语音/触摸/系统状态接入；
- LLM `ai_agent` Skill 和结构化工具；
- [x] Core 主线程直接驱动 Eye Engine，网络线程只提交命令。

### M3：设备控制网关

- [x] 局域网 HTTP/JSON v1 调试网关；
- openvela MCP Server 的 Streamable HTTP transport；
- 电脑 Agent 到 Nyabula 的 MCP 配对、权限和审计；
- [~] 内嵌 HTML 调试控制台已完成，Flutter Client 待实现；
- BLE 离线控制与设备发现；
- [ ] WebSocket 权威状态订阅：snapshot、revision patch、command ack、时钟同步；
- [ ] 鉴权、配对、scope、限流和审计。

### M4：Go 公网母服务

- 账号、设备注册和配对；
- 设备主动 WSS 长连接；
- 状态同步、虚拟猫舍数据和跨设备事件；
- 离线队列、幂等请求和版本协商。

## 8. 当前事实

- 2026-08-14：`sim:lvgl_fb`、LVGL 9 和 Nyabula APP 已完整编译，置信为
  `编译通过`。
- 2026-08-16：在 Debian VM 的 Xvfb/Openbox/x11vnc 环境启动 sim，NSH 中
  执行 `nyabula demo` 后成功打开 `720×360` framebuffer；窗口运行置信为
  `实测确认`。
- 2026-08-17：启用 LVGL Vector Graphic 与 ThorVG internal backend 后在构建机
  Xvfb 中实测；两个独立 `360×360` canvas 的待机、音乐和计时器截图通过，完整
  字体挂载后无 FreeType 错误，常态约 53～61 fps。真机 RK3576 性能仍待双屏
  display/flush 后端完成后测量。
- 当前 sim 仍使用一个宿主 display，但 renderer 已是两个独立 `360×360` canvas；
  真机只需把双 parent API 接到队友的两个 display root，不再重构帧布局。
- Core 首版已实现固定队列、优先级、租约恢复和 typed scene 命令；HTTP/JSON
  网关与内嵌控制台已在 NuttX sim 的 `0.0.0.0:8080` 实测。
- HTML Demo 已覆盖更完整的视觉行为，迁移时以其参数与动效为基准，不复制
  浏览器运行时。
- ST77916 驱动位于团队 Draft PR #59；FSPI 驱动 PR #46 已合并。
