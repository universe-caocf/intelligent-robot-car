#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 主机验证逻辑，不刷机、不联网，临时产物自动清理。
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_dir="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"
cjson_dir="$idf_dir/components/json/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  echo '需要已安装的 ESP-IDF（仅使用其中的 cJSON 源码）；请设置 IDF_PATH。' >&2
  exit 1
fi
for tool in "${CXX:-c++}" "${CC:-cc}" node; do
  command -v "$tool" >/dev/null || { echo "缺少主机测试工具：$tool" >&2; exit 1; }
done
scratch_dir="$(mktemp -d "${TMPDIR:-/tmp}/car-chassis-test.XXXXXX")"
trap 'rm -rf "$scratch_dir"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror)
cflags=(-std=c99)
if [[ "${CAR_TEST_SANITIZE:-0}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" -I "$project_dir/firmware/main" \
  "$project_dir/tests/chassis/state_test.cc" -o "$scratch_dir/state_test"
"$scratch_dir/state_test"
"${CC:-cc}" "${cflags[@]}" -Wno-deprecated-declarations -I "$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$scratch_dir/cJSON.o"
"${CXX:-c++}" "${flags[@]}" -Wno-unused-variable \
  -I "$project_dir/tests/chassis/stubs" -I "$project_dir/firmware/main" -I "$cjson_dir" \
  "$project_dir/tests/chassis/http_test.cc" "$project_dir/firmware/main/web_server/web_server.cc" \
  "$scratch_dir/cJSON.o" -o "$scratch_dir/http_test"
"$scratch_dir/http_test"
"${CXX:-c++}" "${flags[@]}" \
  -include "$project_dir/tests/chassis/adapter_stubs/application.h" \
  -include "$project_dir/tests/chassis/adapter_stubs/settings.h" \
  -include "$project_dir/tests/chassis/adapter_stubs/config.h" \
  -I "$project_dir/tests/chassis/adapter_stubs" -I "$project_dir/tests/chassis/stubs" \
  -I "$project_dir/firmware/main" -I "$cjson_dir" \
  "$project_dir/tests/chassis/adapter_test.cc" "$project_dir/firmware/main/motor_control.cc" \
  "$scratch_dir/cJSON.o" -o "$scratch_dir/adapter_test"
"$scratch_dir/adapter_test"
node "$project_dir/tests/chassis/browser_test.js" "$project_dir/firmware/main/web_server/web_server.cc"
