#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
project_dir=${script_dir:h}
configuration=${1:-debug}

SIGN_IDENTITY="Desk Console Development"

ensure_signing_identity() {
    if security find-certificate -c "$SIGN_IDENTITY" >/dev/null 2>&1; then
        return
    fi
    echo "创建自签名代码签名身份：$SIGN_IDENTITY（仅本机，私钥不进入仓库）"
    local cert_dir
    cert_dir=$(mktemp -d)
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$cert_dir/key.pem" -out "$cert_dir/cert.pem" \
        -days 3650 -nodes -subj "/CN=$SIGN_IDENTITY" \
        -addext "keyUsage=digitalSignature" \
        -addext "extendedKeyUsage=codeSigning" \
        -addext "basicConstraints=critical,CA:FALSE" >/dev/null 2>&1
    openssl pkcs12 -export -out "$cert_dir/identity.p12" \
        -inkey "$cert_dir/key.pem" -in "$cert_dir/cert.pem" \
        -passout pass:desk -legacy >/dev/null 2>&1
    security import "$cert_dir/identity.p12" \
        -k ~/Library/Keychains/login.keychain-db \
        -P desk -T /usr/bin/codesign >/dev/null 2>&1
    rm -rf "$cert_dir"
}

cd "$project_dir"
ensure_signing_identity
swift build -c "$configuration"

binary_path=$(swift build -c "$configuration" --show-bin-path)
app_path="$project_dir/.build/桌面控制台助手.app"

mkdir -p "$app_path/Contents/MacOS"
cp "$binary_path/DeskConsoleHelper" "$app_path/Contents/MacOS/DeskConsoleHelper"
cp "$project_dir/AppInfo.plist" "$app_path/Contents/Info.plist"
# 固定签名身份（自签名证书）：ad-hoc 签名（-s -）每次构建都变，会使钥匙串
# 认证密钥的 ACL 失效（-128），并引发"忘记设备+重新登记"循环。固定身份让
# 重建后钥匙串与蓝牙权限保持稳定。
codesign \
    --force \
    --sign "$SIGN_IDENTITY" \
    --identifier com.dongyu.desk-console-helper \
    --requirements '=designated => identifier "com.dongyu.desk-console-helper"' \
    "$app_path" >/dev/null

echo "$app_path"
