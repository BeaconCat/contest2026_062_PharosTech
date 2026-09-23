# 团队仓归档后的研发入口

2026-09-23：以原团队仓最后的 `dev-ai-contest-2026`（`10f0ecb528e5d60ef9ede628d644350c8fce583f`）为迁移基线，继续在 `BeaconCat/contest2026_062_PharosTech` 开发。yunline 已有该仓写权限。原仓保持归档，原 PR 讨论和 review 原地保留。

## Draft 对照

| 原团队仓 | 续研仓 | 分支 |
|---|---|---|
| [#87](https://github.com/open-vela/contest2026_062_PharosTech/pull/87) | [#1](https://github.com/BeaconCat/contest2026_062_PharosTech/pull/1) | continuation/amp-compute-20260923 |
| [#91](https://github.com/open-vela/contest2026_062_PharosTech/pull/91) | [#2](https://github.com/BeaconCat/contest2026_062_PharosTech/pull/2) | continuation/native-services-20260923 |
| [#93](https://github.com/open-vela/contest2026_062_PharosTech/pull/93) | [#3](https://github.com/BeaconCat/contest2026_062_PharosTech/pull/3) | continuation/product-20260923 |
| 本地 DSP 初版 | [#4](https://github.com/BeaconCat/contest2026_062_PharosTech/pull/4) | continuation/audio-dsp-20260923 |

产品分支包含本地重整的历史和新增研发提交；AMP 带入6个本地新增提交并适配主线构建注册；Core 保持原 head。前三个系列存在共享历史，审核合入时应协调依赖顺序。迁移没有自动合入任何 PR，也不代表 Draft 的功能与 CI 已全部验收。

## 拉取工作区

本变更生效后，团队 manifest 的自身 project 使用 beaconcat remote，避免源码仍从归档仓获取。NuttX 和其他基座依赖继续沿用原来的 manifest 定义。

```sh
repo init -u https://github.com/BeaconCat/contest2026_062_PharosTech \
  -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j4
cd contest2026_062_PharosTech
git fetch https://github.com/BeaconCat/contest2026_062_PharosTech \
  continuation/product-20260923
git switch -c product-development FETCH_HEAD
```

此处切换只针对干净工作区。已有未提交工作应先保留，不执行强制 checkout/reset。
本地主要仓的 `origin` 指向 BeaconCat fork，`upstream` 指向归档团队仓；保留 `BeaconCat` 别名兼容原有脚本。

## 未提交改动的保全

仍在开发的 DSP 已拆为源码/离线工具与 CI 两项提交并开 Draft。
其余旧调试工作区中的源码变更保存在 `archive/local-wip-20260923/*` 分支，按工作区分别建恢复快照；这些分支不作为可直接合入的功能 PR。恢复后应再按单一职责整理提交并验证。

原工作区及原暂存区均保留。已在主线出现的同内容文件不重复提交，Windows symlink 占位文本不作为功能变更提交。编译缓存、镜像、串口日志、Wi-Fi 配置和疑似凭据文本留在本机。

迁移盘点和原始 staged/unstaged 补丁保存在本机 `工具/研发迁移备份/20260923/`，该目录不加入公开仓。代码快照以远端 archive 分支为准。
