#!/usr/bin/env bash
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DESK_IDF_ACTIVATE="${DESK_IDF_ACTIVATE:-/Users/dongyu/.espressif/tools/activate_idf_v5.5.5.sh}"

if [[ ! -f "${DESK_IDF_ACTIVATE}" ]]; then
    echo "未找到 ESP-IDF 激活脚本：${DESK_IDF_ACTIVATE}" >&2
    exit 1
fi

echo "1/3 运行主机侧协议与隐私测试"
"${SCRIPT_DIR}/test-host.sh"

echo "2/3 编译完整固件"
zsh -lc 'source "$1" >/dev/null && idf.py -C "$2" build' _ "${DESK_IDF_ACTIVATE}" "${FIRMWARE_DIR}"

FIRMWARE_BIN="${FIRMWARE_DIR}/build/esp32_desk_console.bin"
if [[ ! -f "${FIRMWARE_BIN}" ]]; then
    echo "编译完成后未找到固件镜像" >&2
    exit 1
fi

echo "3/3 检查固件镜像"
wc -c "${FIRMWARE_BIN}"
echo "准备检查通过。本脚本不会连接、擦除或烧录开发板。"
