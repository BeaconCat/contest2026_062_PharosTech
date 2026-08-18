# Nyabula 本地状态协议 v1

## 1. 边界

Core 是领域状态的唯一真源。HTML、Flutter、LVGL 都是 Renderer，不允许先改本地状态再假设 Core 会接受。
用户操作应提交语义命令；Renderer 收到 Core 的 snapshot/patch 后才改变画面。

Windows Mock、未来 Flutter Eye Stack 与 openvela LVGL Eye Engine 使用相同的字段语义。Canvas、Flutter
CustomPainter 和 LVGL 只负责各自的绘制实现。

## 2. 独立 WebSocket 通道

| 路径 | 内容 | 当前状态 |
|---|---|---|
| `/ws/v1/eyes` | 表情、凝视、眨眼、Scene 权威状态 | 已实现 |
| `/ws/v1/core` | Timer、Task 等服务状态 | 已实现 |
| `/ws/v1/agent-runs` | Agent Run、阶段、工具调用 | 已实现 |
| `/ws/v1/media` | 媒体会话与通话状态 | 已实现 |
| `/ws/v1/telemetry` | 设备状态与遥测 | 已实现 |

每次连接先收到完整 `state.snapshot`。后续变更使用通道内单调递增 revision 的 `state.patch`。各通道有独立连接与 revision，
Agent 大日志或媒体事件不能阻塞 Eye 动画状态。

```json
{
  "protocol": "nyabula.v1",
  "type": "state.snapshot",
  "channel": "eyes",
  "revision": 12,
  "server_time_ms": 1780000000000,
  "state": {}
}
```

当前 patch 是替换整个通道根状态，先保证恢复和跨端一致性；状态增长后再增加字段级 patch，不改变 envelope。

`eyes` 状态中的眨眼事件由 `blink_nonce` 和 `blink_eyes` 共同描述。`blink_nonce` 每次命令递增，
`blink_eyes` 取 `left`、`right` 或 `both`；Renderer 只能闭合指定眼睛，不能把单眼命令退化成双眼眨眼。

```json
{
  "protocol": "nyabula.v1",
  "type": "state.patch",
  "channel": "eyes",
  "revision": 13,
  "patch": {"op": "replace", "path": "/", "value": {}}
}
```

客户端可发送 `state.resume`；Host 返回当前 snapshot。客户端还可发送 `clock.sync`，用返回的
`server_time_ms` 估计墙钟偏移。断线期间 Renderer 保持最后权威状态并按本地单调时钟继续动画，重连后以 snapshot
校正，不逐帧同步动画。

## 3. 命令

WS 命令：

```json
{
  "protocol": "nyabula.v1",
  "type": "command",
  "request_id": "flutter-42",
  "command": {
    "action": "eyes.expression",
    "expression": "curious",
    "source": "flutter",
    "priority": 40,
    "lease_ms": 3000
  }
}
```

同一命令也可提交到 `POST /api/v1/command`。WS 返回 `command.ack`；真正显示结果仍以其后的权威 patch 为准。

已实现的纯逻辑 Service：

- Eye ownership：expression/scene 按 source、priority、sequence、lease 仲裁；
- Timer：`timer.start/pause/resume/cancel`；
- Alarm：`alarm.create/enable/dismiss/snooze/delete`；
- Task：`task.create/update/delete`，多个任务并存；
- Agent Run Mock：`agent.run.start/progress/tool/complete/fail`；
- Media：`media.load/play/pause/seek/stop/view/lyrics`；`media.lyrics` 原子提交三行歌词窗口，避免逐字段 patch 产生半帧错位；
- Call：`call.incoming/answer/end/clear`；
- Device：`device.update` 更新网络、音频、电量、隐私等状态，可用 `focus` 请求限时展示；
- EyePresenter：按 Call > Alarm > Agent > Task > Timer > Media > Device 的优先级自动映射到预定义
  Scene 和表情，较高优先级释放后恢复原有低优先级状态。

Alarm、Media 和 Call 的业务字段会由 EyePresenter 写入 `eyes.scene_payload`。Renderer 应显示这些权威字段，
不得写死联系人、曲目、时长或闹钟文案；缺字段时才可使用视觉占位值。

媒体停止和自然播完先进入 520 ms `stopping` 状态：`media_position_ms` 先以连续动画回到 0，随后才释放
Music Scene。播放/暂停只允许替换左屏中央图标，进度环、标题和时间不得跟随整屏退场。歌词窗口按
`previous_line/current_line/next_line` 一次提交；窗口推进时旧第一行下移并放大为第二行，旧第二行下移并
缩小为第三行，旧第三行向下退出，新第一行从上方落位。

Scene payload 更新同样属于动画状态，不允许闪切。枚举、布尔和文字变化使用围绕屏幕圆心的缩放交叉转场，
旧内容缩小退场、新内容放大入场；进度、倒计时、电量、温度、距离、心率和 EQ 等连续量使用原位插值，避免高频遥测
反复触发整场退场动画。Canvas、Flutter 与 LVGL 应遵守同一语义，但可按各自图形后端实现。

客户端从 `/api/v1/capabilities` 的 `renderer_contract` 读取当前动画契约。`continuous_fields` 使用 520 ms
贝塞尔插值；未列入的 payload 字段默认视为语义字段，使用 420 ms 旧态退出/新态入场。`clock_fields`
只负责把服务端单调时钟锚定到 Renderer 本地时钟，本身不能触发任何视觉转场。该元数据用于约束 HTML、
Flutter 和 LVGL 一致，客户端不能自行猜测字段类型。

`eyes.scene.show` 是视觉调试入口。产品功能应优先调用 Timer/Task/Media 等领域命令，由 EyePresenter 决定显示。

## 4. MCP 双向闭环

入站：局域网 Agent 通过 `POST /mcp/v1` 调用 Nyabula MCP Server。当前提供 initialize、tools/list、tools/call，
并暴露表情、Scene、Task 和通用语义命令工具。

出站：Windows Host 的 `POST /api/v1/mcp/call` 可调用另一个 `http://` MCP 端点，已经能回调本机 MCP Server
形成端到端闭环。该接口只用于开发；产品化前必须补齐 peer 配对、token、scope、审计、限流、超时、取消和 HTTPS。

openvela `ai_agent` 适配暂不接入。Agent Run 数据模型与 WS 已可先供控制台、Flutter 和 Mock Agent 联调，之后只替换
执行适配器，不修改多端状态协议。
