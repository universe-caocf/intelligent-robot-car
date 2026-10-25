# 从 v0 开始开发

`v0` 标识导入的 codebase 快照，保持不动。此前准备工作按 [CODEBASE.md](../CODEBASE.md) 如实保留，后续 PR 既可恢复此前工作，也可新增功能，但不能把恢复工作描述为比赛期间新写的代码。

## 分支关系

- `main`：已验收、可演示或可交付的阶段版本。
- `dev`：已完成单项检查的开发成果，供下一阶段联合验证。
- 功能分支：从最新 `dev` 新建，一次完成一个可独立检查的任务，通过 PR 合入 `dev`。

commit 保存本地改动；push 将提交上传 GitHub；PR 请求将来源分支（compare）合入目标分支（base）。上传分支不会自动合并。

## 第一步：构建工具

分支为 `chore/build-tooling`。范围是工具链安装、显式小车构建预设、编译与烧录入口、忽略规则、依赖锁文件和基础文档。完整固件编译与脚本检查通过后，提交并上传该分支：

```bash
git status --short
git diff
git add .gitignore LICENSE README.md THIRD_PARTY_NOTICES.md docs tools firmware/.gitignore firmware/dependencies.lock
git diff --cached --stat
git diff --cached --check
git commit -m "chore: add reproducible firmware build tooling"
git push -u origin chore/build-tooling
```

在 GitHub 创建 PR，选择 **base: dev，compare: chore/build-tooling**。说明改了什么、如何验证、尚未验证什么；检查 Files changed 中没有凭据、构建缓存或其他模块。若需修正，继续在同一分支提交并 push，原 PR 会自动更新。检查通过后使用 **Create a merge commit** 保留实际提交。

合并后，本地切回并更新 `dev`，再新建下一步分支：

```bash
git fetch origin
git switch dev
git pull --ff-only origin dev
git switch -c chore/car-project-layout
```

切换前确认工作区干净；`--ff-only` 若失败，先查看分叉原因，不要强推或清除本地改动。

## 第二步：单小车工程

第一步合并后，从最新 `dev` 创建 `chore/car-project-layout`。本步骤裁剪无关板型、调整构建菜单与路径、统一工程名称并补齐 CI，范围见 [工程说明](repository.md)。先备份本机配置和旧构建目录，再完整编译，并确认原有源码引用没有丢失。

检查完成后提交并上传，随后创建 **base: dev，compare: chore/car-project-layout** 的 PR：

```bash
git status --short
git diff --stat
git add -A -- .github README.md THIRD_PARTY_NOTICES.md docs firmware tools
git diff --cached --check
git diff --cached --summary
git commit -m "chore: focus firmware project on ESP32-S3 car"
git push -u origin chore/car-project-layout
```

删除文件较多是本步骤的预期结果，但仍需检查范围、目录重命名和许可证保留情况。PR 合并后按前述方式更新 `dev`，再创建 `feat/linx-integration`；此时不需要直接合入 `main`。

## 第四步：底盘控制

分支为 fix/chassis-control，仍从最新 dev 创建并 PR 到 dev。范围包括统一动作执行器、动作替换和停止、网页心跳租约、故障反馈、共享配置，以及关闭自动情绪移动。实现、兼容变化和到货后验收清单见 [底盘控制说明](chassis-control.md)。主机测试直接编译生产状态机、HTTP 与设备适配代码，并执行内嵌网页脚本；这些不等于真实硬件验收。

提交前执行 ./tools/test-chassis.sh、CAR_TEST_SANITIZE=1 ./tools/test-chassis.sh、./tools/build.sh，并检查 git diff --check。此步骤不导入炮塔、不恢复自动全量 CI、不改变 v0。合并后再从更新的 dev 创建 feat/turret-control。

## 第五步：炮塔控制

分支为 feat/turret-control，从已合并底盘的 dev 创建，PR 仍到 dev。范围是默认禁用的软弹机构、独立实体确认、有限次可取消推弹、可选俯仰舵机、全局停车接入和无硬件测试，详见 [炮塔控制](turret-control.md)。新增状态机和驱动适配按当前取消规则重写，没有直接恢复旧准备代码中无法中止的延迟发射任务。

提交前执行 ./tools/test-turret.sh、./tools/test-chassis.sh，并使用 CAR_TEST_SANITIZE=1 运行两套测试；完成默认禁用和临时启用配置的固件编译。测试与构建不代表实物发射已通过。不要提交生成的 sdkconfig 或临时接线配置，不恢复全自动 CI，不移动 v0。合并后从更新的 dev 创建 docs/product-and-architecture，收尾产品、架构、组装和演示文档。

## 后续模块与发布节点

第六步 docs/product-and-architecture 已补齐产品、模块规格/单人分工、接线、分层验收、演示与比赛交付文档。代码与工具的补漏修正可在同一分支形成后续独立commit，逐次保留验证结果，最终通过一个PR合入dev。实机视频和真实验收记录不能提前伪造；硬件前状态以 [验收](acceptance.md) 和最终准备报告为准。

第三步 `feat/linx-integration` 单独提交灵矽默认接入地址、配置来源/设备身份日志和配网绑定说明，PR 仍以 `dev` 为目标。更新 defaults 后用新的有效配置完成本地构建；公开入口可达只算预检查，不代表设备绑定、语音或 MCP 已验收。合并后再从最新 `dev` 创建 `fix/chassis-control`。

| 顺序 | 分支 | 工作范围 |
| --- | --- | --- |
| 1 | `chore/build-tooling` | 工程入口、锁文件、基础说明 |
| 2 | `chore/car-project-layout` | 板型裁剪、项目改名、路径和 CI 调整，重新完整构建 |
| 3 | `feat/linx-integration` | 灵矽接入预置、板级参数和配网说明 |
| 4 | `fix/chassis-control` | 动作覆盖、失联停车、默认参数与状态反馈 |
| 5 | `feat/turret-control` | 炮塔控制、可中止发射和 MCP 工具 |
| 6 | `docs/product-and-architecture` | 产品、模块架构、组装测试和演示材料 |

各模块独立 PR 到 `dev`，不要一次恢复整个旧工作区。文档也应随对应功能及时更新。

总目标是完成硬件到货前所有可完成的开发，不止搭好编译环境。第 1–6 步中不依赖实物的源码、配置、架构、运行说明和模拟验证都应提前完成；到货后集中做接线核对、云端与设备联调、供电测试、动作标定、发射机构验证和实机 demo 录制。

第 2 步允许删除与小车无关的板型、旧项目文件和发布脚本，但应先检查构建规则、头文件引用、资源生成和运行时加载关系，删除后完整构建。不能因为名称属于上游就删除公共音频、通信、显示模块、协议兼容标识或许可证。第 4–5 步应提前验证动作替换、失联停车、停止/解除武装中止发射等状态逻辑；编译通过不能替代这些逻辑检查，也不能替代最终实机验证。

第 1–3 步完成并联合检查后，可以发起 **base: main，compare: dev** 的阶段 PR，明确交付状态是“工程与接入预配置就绪、等待实机验证”。底盘、炮塔与实机验证完成后，再发起功能演示阶段 PR。单个功能完成不需要立刻更新 `main`，也不需要每个 PR 都发布 Release。

阶段 PR 合并后，没有并行开发时这样同步 `dev`：

```bash
git fetch origin
git switch main
git pull --ff-only origin main
git switch dev
git merge --ff-only origin/main
git push origin dev
```

以后每个功能都从同步后的 `dev` 开始，不从旧功能分支继续叠加。保留真实开发过程、PR 和验证记录，不重写既有历史或移动 `v0`。
