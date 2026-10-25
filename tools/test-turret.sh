#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 直接编译生产状态机与适配层，不需要硬件、不发送云端请求。
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_dir="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"
cjson_dir="$idf_dir/components/json/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  echo '需要已有 ESP-IDF 中的 cJSON 源码；可通过 IDF_PATH 指定。' >&2; exit 1
fi
scratch_dir="$(mktemp -d "${TMPDIR:-/tmp}/car-turret-test.XXXXXX")"
trap 'rm -rf "$scratch_dir"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror)
cflags=(-std=c99)
if [[ "${CAR_TEST_SANITIZE:-0}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  cflags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" -I "$project_dir/firmware/main" \
  "$project_dir/tests/turret/state_test.cc" -o "$scratch_dir/state_test"
"$scratch_dir/state_test"
"${CC:-cc}" "${cflags[@]}" -Wno-deprecated-declarations -I "$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$scratch_dir/cJSON.o"
for enabled in 0 1; do
  "${CXX:-c++}" "${flags[@]}" -DCONFIG_CAR_TURRET_ENABLED="$enabled" \
    -include "$project_dir/tests/chassis/adapter_stubs/application.h" \
    -include "$project_dir/tests/chassis/adapter_stubs/settings.h" \
    -include "$project_dir/tests/chassis/adapter_stubs/config.h" \
    -I "$project_dir/tests/chassis/adapter_stubs" -I "$project_dir/tests/chassis/stubs" \
    -I "$project_dir/tests/turret/stubs" -I "$project_dir/firmware/main" -I "$cjson_dir" \
    "$project_dir/tests/turret/adapter_test.cc" "$project_dir/firmware/main/turret_controller.cc" \
    "$project_dir/firmware/main/motor_control.cc" "$scratch_dir/cJSON.o" -o "$scratch_dir/adapter_test"
  "$scratch_dir/adapter_test"
done
