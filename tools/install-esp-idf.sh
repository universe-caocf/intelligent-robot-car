#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS 首次安装；已有安装仅检查版本并补齐工具，不更新或覆盖源码。
set -eo pipefail

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  echo '用法：./tools/install-esp-idf.sh（macOS；可用 IDF_PATH 指定安装目录）'
  exit 0
fi
if [[ $# -ne 0 ]]; then
  echo '不支持的参数；使用 --help 查看用法。' >&2
  exit 2
fi
if [[ "$(uname -s)" != Darwin ]]; then
  echo '本安装入口仅支持 macOS；其他系统请按 docs/build.md 安装 ESP-IDF v5.5.1。' >&2
  exit 1
fi
for command_name in git python3 brew; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "缺少 ${command_name}；请先按 docs/build.md 准备系统依赖。" >&2
    exit 1
  fi
done

idf_dir="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"
if [[ -e "$idf_dir" ]]; then
  # 先检查已有目录，版本不符时退出，避免自动切分支或重装别人的环境。
  if [[ ! -f "$idf_dir/export.sh" ]] ||
     [[ "$(git -C "$idf_dir" describe --tags --exact-match HEAD 2>/dev/null)" != v5.5.1 ]]; then
    echo "已有目录不是 ESP-IDF v5.5.1：${idf_dir}；请通过 IDF_PATH 指定独立安装目录。" >&2
    exit 1
  fi
fi

export HOMEBREW_NO_AUTO_UPDATE=1
brew install cmake ninja dfu-util
if [[ ! -e "$idf_dir" ]]; then
  mkdir -p "$(dirname "$idf_dir")"
  git clone --branch v5.5.1 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$idf_dir"
fi
"$idf_dir/install.sh" esp32s3
echo "安装完成：${idf_dir}；下一步执行 ./tools/build.sh。"
