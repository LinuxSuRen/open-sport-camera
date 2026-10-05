#!/usr/bin/env bash
# 对未签名 HAP 做本地调试签名（需要先在 AGC 申请调试证书 osc_debug.cer 与调试 Profile osc_debug.p7b）
# 用法：bash .signing/sign.sh [未签名HAP路径]
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
SIGN_TOOL="${DEVECO_SDK_HOME:-$HOME/Library/Huawei/command-line-tools/sdk}/default/openharmony/toolchains/lib/hap-sign-tool.jar"
HAP="${1:-$(cd "$DIR/.." && pwd)/entry/build/default/outputs/default/entry-default-unsigned.hap}"
CER="$DIR/osc_debug.cer"
P7B="$DIR/osc_debug_profile.p7b"
P12="$DIR/osc_debug.p12"
OUT="${HAP%.hap}-signed.hap"

# Profile 文件名宽容处理：未按约定命名时取目录里任意 .p7b
if [ ! -f "$P7B" ]; then
  FOUND_P7B="$(ls "$DIR"/*.p7b 2>/dev/null | head -1 || true)"
  if [ -n "$FOUND_P7B" ]; then
    P7B="$FOUND_P7B"
    echo "使用 Profile: $P7B"
  fi
fi

for f in "$CER" "$P7B" "$P12"; do
  if [ ! -f "$f" ]; then
    echo "缺少签名材料: $f（参见 CONTRIBUTING.md 真机调试签名一节）" >&2
    exit 1
  fi
done

# shellcheck disable=SC1091
source "$DIR/passwords.env"

java -jar "$SIGN_TOOL" sign-app \
  -mode localSign \
  -keyAlias oscdebug -keyPwd "$KEY_PWD" \
  -appCertFile "$CER" -profileFile "$P7B" \
  -inFile "$HAP" -keystoreFile "$P12" -keystorePwd "$STORE_PWD" \
  -signAlg SHA256withECDSA -profileSigned 1 \
  -outFile "$OUT"

echo "已生成签名 HAP: $OUT"
echo "安装: hdc install $OUT"
