#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 默认只运行主机验证；--build 才构建固件，不烧录、不登录平台。
set -euo pipefail
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
build=0
for arg in "$@"; do
  case "$arg" in
    --build) build=1 ;;
    --sanitize) export CAR_TEST_SANITIZE=1 ;;
    --help|-h) echo '用法：./tools/verify.sh [--sanitize] [--build]'; exit 0 ;;
    *) echo "未知选项：$arg" >&2; exit 2 ;;
  esac
done
cd "$project_dir"
python3 tools/preflight.py
for script in tools/*.sh; do bash -n "$script"; done
python3 -m unittest discover -s tests -p 'test_*.py'
./tools/test-chassis.sh
./tools/test-turret.sh
if [[ "$build" == 1 ]]; then
  ./tools/build.sh
  python3 tools/preflight.py --require-build
fi
echo 'PASS: requested checks complete; real hardware/cloud/demo remain pending.'
