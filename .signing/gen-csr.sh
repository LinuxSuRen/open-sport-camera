#!/usr/bin/env bash
# 生成本地调试签名密钥与 CSR（产物在本目录，已被 .gitignore 排除）
# 用法：bash .signing/gen-csr.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
SIGN_TOOL="${DEVECO_SDK_HOME:-$HOME/Library/Huawei/command-line-tools/sdk}/default/openharmony/toolchains/lib/hap-sign-tool.jar"
ALIAS="oscdebug"
P12="$DIR/osc_debug.p12"
CSR="$DIR/osc_debug.csr"
PWD_FILE="$DIR/passwords.env"

if [ ! -f "$SIGN_TOOL" ]; then
  echo "未找到 hap-sign-tool.jar，请确认 DEVECO_SDK_HOME 指向 Command Line Tools 的 sdk 目录" >&2
  exit 1
fi

# 首次生成随机密码并保存（本地文件，不入库）
if [ ! -f "$PWD_FILE" ]; then
  KEY_PWD=$(openssl rand -hex 12)
  STORE_PWD=$(openssl rand -hex 12)
  {
    echo "KEY_PWD=$KEY_PWD"
    echo "STORE_PWD=$STORE_PWD"
  } > "$PWD_FILE"
  chmod 600 "$PWD_FILE"
fi
# shellcheck disable=SC1091
source "$PWD_FILE"

if [ ! -f "$P12" ]; then
  java -jar "$SIGN_TOOL" generate-keypair \
    -keyAlias "$ALIAS" -keyAlg ECC -keySize NIST-P-256 \
    -keyPwd "$KEY_PWD" -keystoreFile "$P12" -keystorePwd "$STORE_PWD"
  echo "已生成密钥库: $P12"
fi

java -jar "$SIGN_TOOL" generate-csr \
  -keystoreFile "$P12" -keystorePwd "$STORE_PWD" \
  -keyAlias "$ALIAS" -keyPwd "$KEY_PWD" \
  -signAlg SHA256withECDSA \
  -subject "C=CN,O=linuxsuren,CN=OpenSportCamera" \
  -outFile "$CSR"
echo "已生成 CSR: $CSR"
echo
echo "下一步：到 AppGallery Connect 申请调试证书（上传该 CSR）与调试 Profile（绑定设备 UDID 与包名）"
