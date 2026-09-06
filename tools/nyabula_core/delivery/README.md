# Nyabula Core 交付构建

这些脚本从一个已有的完整 openvela 工作区创建独立副本，并以正式
`nycore_main.c` 完成 sim 的 CMake/Ninja、legacy Make 和 CLI 冒烟验证。

```sh
tools/nyabula_core/delivery/prepare.sh \
  /root/openvela/nycore-sim/vela \
  /path/to/contest2026_062_PharosTech \
  /root/openvela/nyabula-delivery-YYYYMMDD \
  /path/to/verified/sqlite-cache

tools/nyabula_core/delivery/verify.sh \
  /root/openvela/nyabula-delivery-YYYYMMDD
```

第四个参数可省略；离线构建时必须提供含固定版本 `sqlite3.c` 与 `sqlite3.h`
的缓存目录，文件哈希不匹配会失败。输出目录名必须以
`nyabula-delivery-` 开头，且不能预先存在。脚本不会清理、
重配或覆盖源工作区。`JOBS` 可控制并行度；`OPENVELA_TOOLS` 可覆盖构建工具
根目录，默认使用构建机的 `/root/openvela/src/vela/prebuilts/tools`。

`DELIVERY_CMAKE_PASS`、`DELIVERY_MAKE_PASS`、两次
`DELIVERY_SMOKE_PASS` 与最终 `DELIVERY_VERIFY_PASS` 全部出现才算通过。
该冒烟只验证正式 CLI 被正确编入并可执行；完整插件功能仍须运行专项回归。
