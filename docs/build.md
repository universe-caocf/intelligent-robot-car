# 构建与烧录

## 工具与配置分别是什么

ESP-IDF 是乐鑫 ESP32 的开发框架和编译工具链，本项目固定使用 **v5.5.1**。`firmware/main/idf_component.yml` 声明组件依赖范围，`firmware/dependencies.lock` 固定本次使用的版本与校验信息；不要为普通功能开发随意升级依赖。

`sdkconfig.defaults*` 是可共享的默认配置，`sdkconfig` 是 ESP-IDF 生成的本机有效配置，已有有效配置优先于默认值。因此不能只添加 defaults 就认为旧构建目录会自动切换板型。

本项目入口在上游 defaults 基础上叠加 `tools/sdkconfig.defaults.car`，明确选择 **ESP32-S3 / Intelligent Robot Car / SSD1306 128×64**。ESP-IDF 还会加载基础 defaults 对应的 `.esp32s3` 文件。板级源码位于 `firmware/main/boards/intelligent-car/`，沿用原 Qebabe 小车引脚；实际硬件是否匹配仍需核对。OTA 地址和动作逻辑仍是上游状态，灵矽预置在后续 PR 导入。

## 首次安装

macOS 先安装 Apple Command Line Tools、Python 3 和 Homebrew，确保 `git`、`python3`、`brew` 可用。若缺少 Apple 工具，可在自己的终端运行 `xcode-select --install` 并按系统提示完成安装。随后执行：

```bash
./tools/install-esp-idf.sh
./tools/build.sh
```

安装脚本会通过 Homebrew 安装 cmake、ninja、dfu-util，将 ESP-IDF v5.5.1 克隆到 `~/esp/esp-idf-v5.5` 并安装 ESP32-S3 工具链，需要联网、磁盘空间，可能出现系统权限提示。它不会自动更新或切换已有 ESP-IDF 的版本。

已有 v5.5.1 不必重新安装。目录不同可指定：

```bash
IDF_PATH=/absolute/path/to/esp-idf ./tools/build.sh
```

Linux 请按 [ESP-IDF v5.5.1 官方入门文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html) 安装系统依赖和工具链，再通过 `IDF_PATH` 使用同一 Bash 构建入口。Windows 建议使用 WSL2 构建；本步骤只验证 macOS，其他系统与 USB 转发未验证。

首次编译会下载托管组件和资源。输出包括 `firmware/build/intelligent_robot_car.bin`、引导程序、分区表、资源镜像与烧录参数。以后再次运行同一命令会增量编译。此前版本生成的 `xiaozhi.bin` 不再是当前固件输出名。

## 有硬件后烧录

先确认板型、Flash/PSRAM、接线和供电匹配，并在初次联调时断开电机和发射机构的动力电源。当前固件保留上游自动动作，尚未加入本项目的动作控制修正。

macOS 可通过 `ls /dev/cu.*` 查看串口，比较插拔前后出现的设备，再明确指定：

```bash
CAR_PORT=/dev/cu.usbmodemXXXX ./tools/build.sh flash
CAR_PORT=/dev/cu.usbmodemXXXX ./tools/build.sh monitor
```

将示例串口替换为实际设备。Linux 通常为 `/dev/ttyACM0` 或 `/dev/ttyUSB0`，需要串口访问权限。`flash` 先构建，再烧录并进入串口日志；退出日志按 **Ctrl+]**。`monitor` 只读取日志，需要已有构建。

优先使用本入口烧录整套镜像，不要仅凭旧教程给应用 `.bin` 猜测固定偏移。烧录会覆盖对应固件分区；本脚本不执行全片擦除，但这不保证不同固件的 NVS 数据格式兼容。当前没有实机烧录结果。

## 配置与排错

`./tools/build.sh menuconfig` 可打开配置菜单。共享配置变化应整理到受版本管理的默认配置中；不要提交生成的 `sdkconfig` 或账号、token、Wi-Fi 密码。当前入口会拒绝其他芯片、板型和屏幕配置，避免“编译成功但选错板”。

从第一步升级时，旧 `CONFIG_BOARD_TYPE_QEBABE_XIAOCHE` 已改为 `CONFIG_BOARD_TYPE_INTELLIGENT_CAR`，预设文件也已改名。请先按下方步骤备份旧 `sdkconfig` 和 `build/`，再重新生成配置；不要直接在旧配置中猜测修改符号。新配网热点前缀为 `RobotCar`，云端设备类型标识也随板型改为 `intelligent-car`，相关注册与 OTA 兼容性需在接入阶段核验。

如果提示配置不符合预设，先检查 `firmware/sdkconfig` 中的 `CONFIG_IDF_TARGET`、`CONFIG_BOARD_TYPE_*` 和 `CONFIG_OLED_*`。确实需要重新从 defaults 生成时，保留旧文件再构建：

```bash
mkdir -p .local-backups
backup_dir="$(mktemp -d "$PWD/.local-backups/config.XXXXXX")"
mv firmware/sdkconfig "$backup_dir/"
if [ -d firmware/build ]; then mv firmware/build "$backup_dir/"; fi
./tools/build.sh
```

仅在 `firmware/sdkconfig` 存在且已确认需要重置时执行。备份不进入 Git，可能包含私密配置；恢复时先保留当前文件，再将该备份的配置和构建目录放回原位置。不要为解决构建问题直接擦除设备或删除源码、锁文件。

网络失败先检查完整错误日志和组件站点连接，不要首先删除 `dependencies.lock`。锁文件应随显式依赖变更在独立 PR 中更新。上游固件还有编译警告，构建通过不能证明运行逻辑正确。

## 第一步构建工具验证记录（2026-10-25，历史记录）

- macOS、ESP-IDF v5.5.1，通过 `./tools/build.sh` 从新生成的配置和构建目录完成编译与链接。
- 生成配置确认目标为 ESP32-S3、Qebabe Xiaoche、SSD1306 128×64；OTA URL 仍为上游地址。
- 产物 `firmware/build/xiaozhi.bin` 大小为 2,707,296 字节，最小应用分区剩余约 34%。
- 两个脚本通过 Bash 语法检查；帮助、非法参数、未指定串口、缺少 ESP-IDF 等入口检查通过；本地文档链接和 Git 空白检查通过。
- 复用已有托管组件缓存，锁文件未被构建改写；没有验证全新联网下载全部组件，也没有重新运行系统安装脚本。
- 固件功能源码保持 v0 内容；已有弃用 API、宏重定义和未使用变量等编译警告仍在。
- 未烧录设备，未验证灵矽绑定、音频、屏幕、电机、供电或发射机构；Linux/Windows 未验证。

第二步改名与裁剪后的构建验证见 [工程验证记录](repository.md#第二步验证记录2026-10-25)，当前产物为 `intelligent_robot_car.bin`。
