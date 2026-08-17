# Nyabula Core 局域网控制协议 v1

## 设计边界

控制台、未来的 Flutter Client、BLE、MCP、语音与 LLM 都只能向 Nyabula
Core 提交语义命令，不能直接调用 LVGL renderer。HTTP 线程只负责校验 JSON
并写入固定容量队列；队列消费、租约仲裁和所有 Eye Engine 调用均发生在 LVGL
主线程。

当前 HTTP 服务是开发调试入口，无身份验证，只应运行在可信局域网。正式产品需在
协议不变的前提下增加配对 token、scope、限流和审计。

## 端点

- `GET /`：内嵌 HTML 调试控制台；
- `GET /api/v1/health`：进程和 API 版本探活；
- `GET /api/v1/state`：Core 当前生效状态、所有者、剩余租约与最后命令结果；
- `GET /api/v1/capabilities`：动作、表情、场景与协议限制；
- `POST /api/v1/command`：校验并异步提交命令；
- `OPTIONS *`：浏览器 CORS 预检。

## 命令信封

```json
{
  "id": "flutter-42",
  "source": "flutter",
  "priority": 50,
  "lease_ms": 3000,
  "action": "eyes.expression",
  "params": {
    "expression": "curious",
    "transition_ms": 280
  }
}
```

- `source` 标识命令来源，同一来源会更新自己的恢复槽；
- `priority` 范围为 0–255，数值越大优先级越高；
- `lease_ms=0` 表示持久租约，否则到期后自动恢复下一优先级状态；
- 同优先级由最新 sequence 获胜；
- HTTP `202` 只表示命令已入队，最终执行结果见 `state.last_command`。

## 动作

| action | params |
|---|---|
| `eyes.expression` | `expression`, `transition_ms` |
| `eyes.blink` | `eyes`: `left/right/both` |
| `eyes.gaze` | `x`, `y`: -1..1；`hold_ms` |
| `eyes.auto_blink` | `enabled` |
| `eyes.ambient` | `level`: 0..1 |
| `eyes.iris` | `eyes`, `rgb`: 24 位十六进制 |
| `eyes.scene.show` | `scene`, `style`, `payload` |
| `eyes.scene.update` | 完整 `payload`，更新同一 source 的当前场景 |
| `eyes.scene.hide` | 无；结束当前 `source` 的 Scene，不阻塞其他来源 |
| `core.release` | `domain`: `expression/scene/all` |
| `core.reset` | 无 |

表情和场景枚举以 `GET /api/v1/capabilities` 为协议真源。

`eyes.scene.hide` 与 `core.release(domain=scene)` 都只释放命令信封中同一
`source` 的 Scene。隐藏不是高优先级的永久黑名单；系统若需要临时压制 Scene，
应提交一个有明确 lease 的策略意图，不能留下无限期 tombstone。

## Scene payload

场景 payload 与 `nyabula_eye_scene_payload_s` 一一对应，包含：

- 枚举：`weather`、`music_view`、`battery_state`、`alarm_copy`、
  `call_state`、`task_state`、`network_state`、`audio_route`、`eq_view`；
- 时间和计数：`duration_ms`、`position_ms`、`remaining_ms`、`elapsed_ms`、
  `year/month/day/hour/minute`、`percent`、`device_count`、
  `briefing_index/count`；
- 浮点参数：`temperature_c`、`feels_like_c`、`humidity_percent`、
  `wind_kph`、`visibility_km`、`distance_m`、`heart_rate_bpm`、
  `crossover_hz`、`progress`；
- EQ：`eq_bands`，固定十段；
- 布尔状态：`active`、`playing`、`privacy_camera`、
  `privacy_microphone`、`signal_good`；
- 文本：`title`、`subtitle`、`detail`、`value`、`previous_line`、
  `current_line`、`next_line`。

首版 `scene.update` 使用完整 payload 替换，避免不同客户端对“字段缺失”的语义产生
分歧。Flutter 数据类应保持同一字段名与枚举字符串，之后可直接替换 HTML 调试端。

## 模拟器网络

sim 使用 `SIM_NETUSRSOCK` 把 NuttX usrsock 请求映射到宿主原生 socket。Core、
HTTP、JSON 解析和 Eye Engine 仍全部运行于 NuttX 进程；Debian 只提供显示和网络
传输，不承载产品逻辑。默认监听 `0.0.0.0:8080`。
