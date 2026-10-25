# 第三方代码与许可声明

本仓库是独立维护的派生项目，独立仓库不改变已有代码的来源、版权或许可证。

## 固件 codebase

- 来源：[Rakonx/xiaozhi-esp32-car](https://github.com/Rakonx/xiaozhi-esp32-car)。
- 原始提交：`94969e50e892fecc3f682385fb6a88163271154a`，导入过程见 [CODEBASE.md](CODEBASE.md)。
- 主体原始许可为 MIT，完整文本保留在 [firmware/LICENSE](firmware/LICENSE)，包含 Shenzhen Xinzhi Future Technology Co., Ltd. 和 Project Contributors 的版权声明。
- 本项目目前增加了工程工具、构建预设、依赖锁文件和文档，并完成单板型裁剪、项目/板型改名与配网热点前缀调整。灵矽预置、底盘控制修正与炮塔尚未导入。完整导入基线保留在 `v0`；裁剪后的路径见 [工程说明](docs/repository.md)。

## 嵌入的第三方源码

| 代码 | 许可证 | 声明位置 |
| --- | --- | --- |
| FluxGarage RoboEyes，Dennis Hoelscher，2024–2025 | GPL-3.0-or-later | `firmware/main/third_party/roboeyes/src/FluxGarage_RoboEyes.h`、该库 `LICENSE.txt` |
| Adafruit GFX，Adafruit Industries，2012 | BSD-2-Clause | `firmware/main/third_party/adafruit_gfx/license.txt` |
| gifdec | Public Domain | `firmware/main/display/lvgl_display/gif/LICENSE.txt` |

RoboEyes 已被 OLED 适配代码包含并编入固件。本项目新增代码与组合固件按 GPL-3.0-or-later 分发，完整文本见根目录 [LICENSE](LICENSE)。这不改变原有 MIT、BSD 等代码单独适用的许可。分发固件时，应提供对应版本的完整源码、构建说明及相关许可声明。

ESP-IDF、`firmware/main/idf_component.yml` 中的外部组件和资源适用其各自随附许可。依赖锁文件记录版本与校验信息，不能替代各组件的许可证。上表仅列出已识别的内嵌库；完整 codebase 中其他文件和资源的随附声明同样需要保留。
