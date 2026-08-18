#!/bin/zsh

set -euo pipefail

script_dir=${0:A:h}
project_dir=${script_dir:h}
configuration=${1:-debug}

cd "$project_dir"
swift build -c "$configuration"

binary_path=$(swift build -c "$configuration" --show-bin-path)
app_path="$project_dir/.build/桌面控制台助手.app"

mkdir -p "$app_path/Contents/MacOS"
cp "$binary_path/DeskConsoleHelper" "$app_path/Contents/MacOS/DeskConsoleHelper"
cp "$project_dir/AppInfo.plist" "$app_path/Contents/Info.plist"
codesign \
    --force \
    --sign - \
    --identifier com.dongyu.desk-console-helper \
    --requirements '=designated => identifier "com.dongyu.desk-console-helper"' \
    "$app_path" >/dev/null

echo "$app_path"
