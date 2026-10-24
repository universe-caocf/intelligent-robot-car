#!/bin/bash
# 安装 ESP-IDF v5.5（Rakonx 固件 CI 使用的版本）到 ~/esp/esp-idf-v5.5
set -e
export HOMEBREW_NO_AUTO_UPDATE=1
echo "== 1/3 brew 安装 cmake ninja dfu-util =="
brew install cmake ninja dfu-util 2>&1 | tail -3
mkdir -p ~/esp
cd ~/esp
if [ ! -d esp-idf-v5.5 ]; then
  echo "== 2/3 克隆 esp-idf v5.5.1 =="
  git clone -b v5.5.1 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v5.5 \
    || git clone -b release/v5.5 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v5.5
fi
cd esp-idf-v5.5
echo "== 3/3 安装 esp32s3 工具链 =="
./install.sh esp32s3
echo "== 安装完成 =="
echo "以后每次打开终端先执行：  . ~/esp/esp-idf-v5.5/export.sh"
