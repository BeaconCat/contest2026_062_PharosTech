# 集成基线

- 团队PR #83：13aebd5a6919cbeab08342d79d814bdae4a40cca。
- N-Boot PR #7：27eddbe463a2dca688db5edfde965f3d0f396b5e。
- NuttX：e02f581e235fc7b527d57ff62b668ce625d139ab及附带GIC补丁。
- Linux：K7 Android14 SDK b553a938ddb56541f86507050f51af76c9929beb的kernel-6.1。

原分支不改写，AMP分别在新分支开发。团队保留最新LCD late initialize、
nbootctl与manifest链接；N-Boot保留#7的早期DM、串口恢复与持久一次性请求。

已提交基础集成是显式RAM FIT启动与真实health/info；本地后续分支增加
默认活动AMP槽启动、试次消费、普通NuttX回退与可选NPU矩阵乘。
这些后续改动尚未推送，不能把基础提交号当作包含全部功能的可复现版本。
新N-Boot proper仍用团队工具包装成完整vendor FIT，再交给nbootctl更新。
不复制Linux GPL源码进Apache BSP；团队仅携带Linux配置/DTS与构建工具。

公共NuttX仓暂不另开PR。GIC补丁随团队仓提供，在CI与独立构建工作区显式应用；
后续提交公共仓并合入后再移除这项临时依赖。

镜像发布与组件许可单独管理，见DISTRIBUTION.md。已有PR或本地测试通过
均不构成自主合入授权，所有远端合入须用户明确指定。
