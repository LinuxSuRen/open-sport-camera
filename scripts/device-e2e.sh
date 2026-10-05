#!/usr/bin/env bash
# 真机端到端验证：签名 → 安装 → 启动 → 采集日志/截图
# 前置：.signing/ 下已有 osc_debug.cer / osc_debug_profile.p7b（AGC 申请）
# 用法：bash scripts/device-e2e.sh [step]
#   step 可选：sign | install | launch | logs | shot | all（默认 all）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HDC="${HDC:-$HOME/Library/Huawei/command-line-tools/sdk/default/openharmony/toolchains/hdc}"
HAP="$ROOT/entry/build/default/outputs/default/entry-default-unsigned.hap"
SIGNED="${HAP%.hap}-signed.hap"
BUNDLE="com.linuxsuren.opensportcamera"
ABILITY="EntryAbility"
OUT_DIR="$ROOT/.signing/e2e"
STEP="${1:-all}"

mkdir -p "$OUT_DIR"

step_build() {
  export PATH="$(dirname "$(dirname "$(dirname "$(dirname "$HDC")")")")/bin:$PATH"
  export DEVECO_SDK_HOME="$(dirname "$(dirname "$(dirname "$(dirname "$HDC")")")")/sdk"
  cd "$ROOT"
  hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
}

step_sign() {
  bash "$ROOT/.signing/sign.sh" "$HAP"
}

step_install() {
  "$HDC" install -r "$SIGNED"
}

step_launch() {
  "$HDC" shell aa start -a "$ABILITY" -b "$BUNDLE"
}

step_logs() {
  echo "== 抓取应用日志（10s）=="
  timeout 10 "$HDC" hilog | grep -iE "CameraService|RecorderService|BurnService|burn|osc" | tee "$OUT_DIR/hilog.txt" || true
}

step_shot() {
  local name="${1:-shot}"
  "$HDC" shell snapshot_display -f "/data/local/tmp/${name}.jpeg" >/dev/null
  "$HDC" file recv "/data/local/tmp/${name}.jpeg" "$OUT_DIR/${name}.jpeg" >/dev/null
  echo "截图: $OUT_DIR/${name}.jpeg"
}

case "$STEP" in
  build) step_build ;;
  sign) step_sign ;;
  install) step_install ;;
  launch) step_launch ;;
  logs) step_logs ;;
  shot) step_shot manual ;;
  all)
    "$HDC" list targets | grep -qE "^[0-9A-F]+" || { echo "未检测到设备" >&2; exit 1; }
    step_build
    step_sign
    step_install
    sleep 2
    step_launch
    sleep 6
    step_shot launch
    echo
    echo "== 已启动，手动操作：允许权限 → 开始录制 → 计圈 → 停止 =="
    step_logs
    ;;
  *) echo "未知步骤: $STEP" >&2; exit 1 ;;
esac
