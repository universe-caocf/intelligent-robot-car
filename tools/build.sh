#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 统一从仓库入口编译本项目小车板型；不自动清除配置或猜测烧录串口。
set -eo pipefail

usage() {
  echo '用法：./tools/build.sh [build|flash|monitor|menuconfig|--help]'
  echo 'ESP-IDF：IDF_PATH（默认 ~/esp/esp-idf-v5.5），必须为 v5.5.1'
  echo '烧录/日志：CAR_PORT=/dev/cu.xxx ./tools/build.sh flash（或 monitor）'
}

action="${1:-build}"
if [[ $# -gt 1 ]]; then usage >&2; exit 2; fi
case "$action" in
  -h|--help) usage; exit 0 ;;
  build|flash|monitor|menuconfig) ;;
  *) usage >&2; exit 2 ;;
esac

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$project_dir/firmware"
idf_dir="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"
if [[ "$action" == flash || "$action" == monitor ]]; then
  if [[ -z "${CAR_PORT:-}" ]]; then
    echo '请用 CAR_PORT 显式指定串口；macOS 可运行 ls /dev/cu.* 查看设备。' >&2
    exit 1
  fi
fi
if [[ ! -f "$idf_dir/export.sh" ]]; then
  echo "未找到 ESP-IDF：${idf_dir}；先运行安装脚本或设置 IDF_PATH。" >&2
  exit 1
fi
. "$idf_dir/export.sh" >/dev/null
idf_version="$(idf.py --version)"
if [[ "$idf_version" != 'ESP-IDF v5.5.1' ]]; then
  echo "需要 ESP-IDF v5.5.1，当前为 ${idf_version}；请检查 IDF_PATH。" >&2
  exit 1
fi

check_config() {
  # 已有 sdkconfig 优先于 defaults；发现旧板型时停止，保留用户配置供排查。
  local expected
  for expected in 'CONFIG_IDF_TARGET="esp32s3"' \
      'CONFIG_BOARD_TYPE_INTELLIGENT_CAR=y' 'CONFIG_OLED_SSD1306_128X64=y'; do
    if ! grep -Fxq "$expected" "$firmware_dir/sdkconfig"; then
      echo "配置不符合当前构建预设：缺少 ${expected}。请参考 docs/build.md 备份并排查配置。" >&2
      exit 1
    fi
  done
  # 无效硬件配置仍允许进入菜单修正；构建和烧录前必须校验。
  if [[ "$action" == build || "$action" == flash ]]; then
    python3 "$project_dir/tools/configure-hardware.py" --check-sdkconfig "$firmware_dir/sdkconfig"
  fi
}
if [[ -f "$firmware_dir/sdkconfig" ]]; then check_config; fi
export IDF_TARGET=esp32s3
idf_args=(-C "$firmware_dir" -B "$firmware_dir/build"
  "-DSDKCONFIG_DEFAULTS=$firmware_dir/sdkconfig.defaults;$project_dir/tools/sdkconfig.defaults.car")
case "$action" in
  build|flash)
    idf.py "${idf_args[@]}" build
    check_config
    if [[ "$action" == flash ]]; then
      idf.py "${idf_args[@]}" -p "$CAR_PORT" flash monitor
    fi
    ;;
  monitor)
    if [[ ! -f "$firmware_dir/build/project_description.json" ]]; then
      echo '尚无构建信息，请先执行 ./tools/build.sh。' >&2
      exit 1
    fi
    idf.py "${idf_args[@]}" -p "$CAR_PORT" monitor
    ;;
  menuconfig)
    idf.py "${idf_args[@]}" menuconfig
    python3 "$project_dir/tools/configure-hardware.py" --check-sdkconfig "$firmware_dir/sdkconfig"
    ;;
esac
