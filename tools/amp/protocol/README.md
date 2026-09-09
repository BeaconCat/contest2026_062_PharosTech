# nyamp v1 控制协议

`nyamp` 是 openvela 控制域与 Linux 计算域之间的应用层协议。它运行在 RPMsg
endpoint上；本层定义版本、服务、请求关联、deadline、取消与generation字段。
当前仅实现最小health/info，不提供独立重启隔离或权限代理。

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
response。`request_id=0`和`service=0`非法。Linux每次服务进程启动生成非零
generation；客户端检查当前请求ID与应答格式，不把它当成安全身份。

HEALTH opcode 0是READY事件，request_id=1、payload为空、generation非零。
服务端打开端点后主动发送，供OpenAMP学习Linux动态地址；客户端忽略其应用负载。
opcode 1查询健康状态，opcode 2返回Linux在线CPU与CPU part。

主机测试：

```sh
cmake -S tools/amp/protocol -B out/nyamp-protocol
cmake --build out/nyamp-protocol
ctest --test-dir out/nyamp-protocol --output-on-failure
```

该层只验证消息形状；插件权限、共享大块数据、独立重启的生命周期均不在本次实现中。
