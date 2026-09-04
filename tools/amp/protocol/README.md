# nyamp v1 控制协议

`nyamp` 是 openvela 控制域与 Linux 计算域之间的应用层协议。它运行在 RPMsg
endpoint 上；RPMsg 负责可靠传输，本层负责版本、服务、请求关联、deadline、取消和
Linux 重启后的 generation 隔离。

所有整数固定为 little-endian，不能直接把 C struct 强转到线上。40 字节头布局：

| offset | 类型 | 字段 |
|---:|---|---|
| 0 | u32 | magic `NYAP` |
| 4 | u16 | version |
| 6 | u16 | header size |
| 8 | u16 | service |
| 10 | u16 | opcode |
| 12 | u32 | flags |
| 16 | u64 | request id |
| 24 | u64 | monotonic deadline（ms） |
| 32 | u32 | compute-domain generation |
| 36 | u32 | inline payload size |

RPMsg 可用 payload 是 496 字节，因此 v1 inline payload 最大 456 字节。图像、音频、
tensor、模型等大对象只能在 payload 中携带共享内存描述符，不得分片硬塞 RPMsg。

每条消息必须且只能是 request/response/event/cancel 之一；error 只允许附加在
response。`request_id=0` 和 `service=0` 非法。Linux 每次启动随机或单调增加
generation，openvela 丢弃旧 generation 的 response、event 和 buffer handle。

主机测试：

```sh
cmake -S tools/amp/protocol -B out/nyamp-protocol
cmake --build out/nyamp-protocol
ctest --test-dir out/nyamp-protocol --output-on-failure
```

该层只验证消息形状，不代替 Permission Broker。插件不能直接取得 RPMsg endpoint；
Core 必须按调用者 capability、目标资源和操作做授权，再生成 nyamp request。
