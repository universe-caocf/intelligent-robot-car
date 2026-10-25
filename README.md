# Intelligent Robot Car

面向公司内部项目比赛的语音交互小车项目，目标是结合 ESP32-S3、七牛灵矽和软弹互动机构。

目前处于从 `v0` codebase 逐步开发的阶段。本步骤仅补齐开发工具和构建说明，尚未导入本项目的灵矽预置、底盘控制修正或炮塔功能。编译成功也不代表硬件验证完成。

## 开始开发

macOS 首次使用：

```bash
git clone https://github.com/universe-caocf/intelligent-robot-car.git
cd intelligent-robot-car
./tools/install-esp-idf.sh
./tools/build.sh
```

已有 ESP-IDF **v5.5.1** 时可以跳过安装，用 `IDF_PATH` 指定目录。无需硬件即可编译；首次构建需要网络和组件下载空间。构建入口明确选择上游原有的 Qebabe ESP32-S3 小车板型与 SSD1306 128×64 屏幕，实际硬件是否匹配仍需到货核对。

- [构建、烧录和排错](docs/build.md)
- [分支与 PR 开发流程](docs/development.md)
- [codebase 来源和基线说明](CODEBASE.md)
- [第三方许可声明](THIRD_PARTY_NOTICES.md)

## 目录与许可

`firmware/` 是普通源码目录，保留完整上游 codebase，不需要初始化 Git 子模块。`tools/` 存放本项目的工程入口，`docs/` 存放新增文档。构建产物、本机配置与下载缓存不提交；组件版本通过 `firmware/dependencies.lock` 固定。

原固件主体的 MIT 声明保留在 [firmware/LICENSE](firmware/LICENSE)。组合固件包含 GPL-3.0-or-later 的 RoboEyes，本项目新增代码和组合固件按 GPL-3.0-or-later 分发，见 [LICENSE](LICENSE)。原有第三方代码保留各自的版权和许可。
