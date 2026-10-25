# Intelligent Robot Car

面向公司内部项目比赛的语音交互小车项目，目标是结合 ESP32-S3、七牛灵矽和软弹互动机构。

目前处于从 `v0` codebase 逐步开发的阶段，已补齐开发工具、收敛为 ESP32-S3 单小车工程，并预置灵矽 OTA/接入地址与接入排错日志。底盘统一执行器、遥控租约与无硬件测试已补齐；炮塔控制、实体确认和可取消推弹序列已实现，默认禁用硬件；云端绑定、语音、MCP 和实车动作仍待验证。

## 开始开发

macOS 首次使用：

```bash
git clone https://github.com/universe-caocf/intelligent-robot-car.git
cd intelligent-robot-car
./tools/install-esp-idf.sh
./tools/build.sh
```

已有 ESP-IDF **v5.5.1** 时可以跳过安装，用 `IDF_PATH` 指定目录。无需硬件即可编译；首次构建需要网络和组件下载空间。当前板型从上游 Qebabe 小车派生，使用 ESP32-S3 与 SSD1306 128×64 屏幕，实际硬件是否匹配仍需到货核对。

- [构建、烧录和排错](docs/build.md)
- [灵矽接入、配网与首轮验证](docs/linx-setup.md)
- [分阶段智能体提示词](docs/agent-prompt.md)
- [底盘规则、状态接口与主机测试](docs/chassis-control.md)
- [炮塔配置、实体确认与无硬件测试](docs/turret-control.md)
- [分支与 PR 开发流程](docs/development.md)
- [工程目录、裁剪范围与兼容说明](docs/repository.md)
- [codebase 来源和基线说明](CODEBASE.md)
- [第三方许可声明](THIRD_PARTY_NOTICES.md)

## 目录与许可

`firmware/` 是普通源码目录，不需要初始化 Git 子模块。`firmware/main/boards/intelligent-car/` 存放小车板级源码；无关板型已移除，完整导入快照保留在 `v0`。`tools/` 存放工程入口，`docs/` 存放项目文档。构建产物、本机配置与下载缓存不提交；组件版本通过 `firmware/dependencies.lock` 固定。

原固件主体的 MIT 声明保留在 [firmware/LICENSE](firmware/LICENSE)。组合固件包含 GPL-3.0-or-later 的 RoboEyes，本项目新增代码和组合固件按 GPL-3.0-or-later 分发，见 [LICENSE](LICENSE)。原有第三方代码保留各自的版权和许可。
