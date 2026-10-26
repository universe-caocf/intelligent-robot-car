# 比赛交付与发布核对

## 要求对应位置

| 比赛要求 | 本仓库位置 | 当前状态 |
| --- | --- | --- |
| 产品所有源代码 | firmware/、tools/、tests/ | 已有源码，按模块PR迭代 |
| 如何运行 | README.md、build.md、linx-setup.md | 开发、烧录、云端与分层验收说明 |
| 架构含模块规格/分工 | architecture.md | 单人职责如实记录，不虚构团队 |
| 产品设计与商业价值 | product.md | 场景、用户、价值假设及验证方法 |
| 实机demo视频上传GitHub | demo/README.md、demo.md | 待实机录制并填真实链接 |
| 每次改动通过PR | development.md、Git历史 | 功能分支→dev，里程碑dev→main |
| codebase前打v0 | CODEBASE.md、v0与既有历史 | 保留补建基线事实，不移动tag或改写时间线 |

本项目确有codebase。v0保留上游导入快照，CODEBASE.md记录2026-10-25补建以及此前已有准备工作；不能把恢复的旧准备代码说成比赛期间全部新写，也不能仅凭补建tag自行保证满足主办方时间要求。对规则有疑问时提供真实Git/PR记录由主办方判断，不删除历史规避。

## 提交与合并

第六步及后续收尾提交保留在 docs/product-and-architecture 分支，同一PR可有多次明确提交。先查看差异、测试与脱敏，再push并向dev提PR。未验收实机时不要把版本描述成“实机全部通过”。

~~~bash
git status --short --branch
git log --oneline origin/dev..HEAD
git diff --stat origin/dev...HEAD
git push -u origin docs/product-and-architecture
~~~

合并到dev后，核对软件准备与待实机清单；只有达到阶段交付标准才从dev向main提里程碑PR。不修改v0，不强推、重置或一次性重做旧历史。没有新的实机结果时不需要再发布一个假装已经验收的Release。

## 正式交付前

- [ ] 仓库访问权限已确认，评委能看到源代码、PR历史和文档。
- [ ] 来源与许可证保留，说明哪些是codebase、哪些是恢复准备、哪些是后续新增。
- [ ] 工具与固件检查通过，硬件验收记录对应实际commit。
- [ ] 已填写实际型号、接线、标定参数、云端协议和未通过项。
- [ ] 已录制并上传真实demo，视频入口可访问且不含凭据。
- [ ] 产品、架构、运行说明与最终固件行为一致。

其中涉及真实账号、器件、访问权限与录像的项目必须由实际证据完成，不能以主机模拟勾选。
