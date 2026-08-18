# Nyabula Windows Host

该目录用于在 Windows 上快速打磨 Nyabula Core、Service、协议和多端显示逻辑。它不是另一套产品后端：
`nyabula_runtime` 只使用 C++17 标准库，后续与 openvela APP 共用；`windows_gateway.cxx`、文件持久化和
`mock_eye.html` 是可替换的 Windows 开发适配层。

原始视觉金标准 `工具/cat_eyes_demo.html` 不在这里修改。`web/mock_eye.html` 是它的独立复制版本，
只额外加载 `mock_eye_transport.js`，通过权威状态协议驱动画面。Mock Eye 是纯 Renderer，不包含按钮、
本地点击或独立业务状态；`web/console.html` 是独立单页控制台，所有操作均由内联 JavaScript 请求 Core。

## 构建

```powershell
cmake -S app/nyabula/host -B app/nyabula/host/build -G "Visual Studio 17 2022" -A x64
cmake --build app/nyabula/host/build --config Release -j 8
ctest --test-dir app/nyabula/host/build -C Release --output-on-failure
```

运行：

```powershell
app\nyabula\host\build\Release\nyabula_host.exe 8090
```

- 控制台：`http://127.0.0.1:8090/`（`/console` 同址）
- Mock Eye：`http://127.0.0.1:8090/mock-eye`（纯显示，可另开窗口）
- 健康检查：`http://127.0.0.1:8090/api/v1/health`
- 入站 MCP：`http://127.0.0.1:8090/mcp/v1`
- 出站 MCP 调试接口：`POST /api/v1/mcp/call`

默认数据目录是 `%LOCALAPPDATA%\Nyabula`，测试时可用 `NYABULA_DATA_DIR` 覆盖。恢复策略明确区分：

- 运行中的 Timer 按 wall-clock deadline 恢复，过期则完成；
- Alarm 保留触发时间、开关和状态，恢复后由 wall clock 继续判断；
- 播放中的 Media 恢复为 `paused` 并标记 `host_restarted`，避免重启后擅自出声；
- 运行中的 Task 回到 `queued`，并标记 `host_restarted`；
- 运行中的 Agent Run 变为 `interrupted`，不会假装继续执行；
- Eye lease、凝视、连接、瞬时 revision 不持久化。

当前纯逻辑 Service 包括 Timer、Alarm、Task、Agent Run Mock、Media、Call 和 Device。EyePresenter 按
Call > Alarm > Agent > Task > Timer > Media > Device 的优先级把服务状态映射为表情与 Scene，客户端无需
直接编排显示冲突。

协议详见 [PROTOCOL.md](PROTOCOL.md)。
