#!/usr/bin/env bash
# 宿主机运行烧录引擎核心单元测试（无需 OHOS SDK / 设备）
# 用法：bash entry/src/main/cpp/tests/run_host_tests.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${TMPDIR:-/tmp}/osc_host_tests"

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -Wall -Wextra -I "$DIR/.." \
  "$DIR/test_main.cpp" "$DIR/../mini_json.cpp" "$DIR/../overlay_layout.cpp" "$DIR/../blitter.cpp" \
  -o "$OUT"

"$OUT"
