# Storage 当前契约与持久化边界

Storage由QuickJS和WAMR共享Broker执行。每个插件的`storage_root/data`为私有键目录，已安装版本共用同一个根目录。

## 已实现的资源限制

- 单值上限：`NYABULA_CORE_STORAGE_VALUE_LIMIT`，默认4096字节。
- 所有值的逻辑总字节上限：`NYABULA_CORE_STORAGE_BYTES_LIMIT`，默认65536字节。
- 键数量上限：`NYABULA_CORE_STORAGE_KEYS_LIMIT`，默认64。

写入前扫描实际目录，不依赖会丢失或漂移的内存计数。覆盖同一个键时扣除其旧长度，按新长度核算；零长度键仍占用一个名额。读写和预算检查受同一进程内的Broker锁保护，并发写入无法分别基于旧额度同时通过。不同插件根目录独立核算。

超出单值上限返回EFBIG；超出字节或键数上限返回EDQUOT。配额拒绝发生在打开目标写入之前，旧值保持不变。此处约束的是逻辑数据量，文件系统元数据、分配簇和包版本空间不计入该数值。

读取在打开前检查lstat，打开后检查fstat；只允许有界普通文件。完整处理短读和EINTR，提前EOF报告EIO，文件增长报告EFBIG；错误路径清空输出指针和长度。显式lstat是必要的：当前NuttX sim的O_NOFOLLOW链路曾实测跟随符号链接。

目录必须由Core独占管理。进程内锁和lstat检查不构成对其他具有原生文件系统权限进程的隔离；最终跨进程架构仍需由唯一Broker管理文件访问。

## 尚未提供的保证

当前写入仍为O_TRUNC。磁盘满、写入错误或断电时不能保证旧值保留；只有配额拒绝和参数/权限拒绝保证写入前返回。

当前NuttX `fs/vfs/fs_rename.c`的mountptrename在调用文件系统rename之前先unlink目标，FAT实现依赖此行为。因此“写临时文件、fsync、rename覆盖”不能被宣称为原子更新；这一限制同样需要审查授权库、撤销库和包marker的替换操作。

下一步应优先验证工程已包含的SQLite事务能力及其NuttX VFS/锁/同步路径，或补齐明确的文件系统原子替换契约。在后端选型和断电/写失败验证完成前，不将Storage或包marker标记为持久化原子性完成。
