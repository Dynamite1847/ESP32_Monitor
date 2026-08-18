#!/usr/bin/env bash
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DESK_IDF_ACTIVATE="${DESK_IDF_ACTIVATE:-${HOME}/.espressif/tools/activate_idf_v5.5.5.sh}"

if [[ ! -f "${DESK_IDF_ACTIVATE}" ]]; then
    echo "未找到 ESP-IDF 激活脚本：${DESK_IDF_ACTIVATE}" >&2
    exit 1
fi

UI_FONT_SOURCE="${FIRMWARE_DIR}/components/ui/fonts/desk_ui_font_16.c"
if [[ ! -f "${UI_FONT_SOURCE}" ]]; then
    echo "未找到界面字体，请先运行 ./scripts/generate-ui-font.sh" >&2
    exit 1
fi

missing_glyphs=""
while IFS= read -r glyph; do
    if ! rg --quiet --fixed-strings -- "${glyph}" "${UI_FONT_SOURCE}"; then
        missing_glyphs="${missing_glyphs}${glyph}"
    fi
done < <(
    while IFS= read -r source_file; do
        rg --no-filename -oP '[\x{3400}-\x{9FFF}]' "${source_file}" || true
    done < <(
        rg --files \
            "${FIRMWARE_DIR}/components/ui" \
            "${FIRMWARE_DIR}/components/app_model" \
            -g '*.c' -g '*.h' -g '*.txt' -g '!desk_ui_font_16.c' -g '!desk_ui_cjk_font_16.c'
    ) | LC_ALL=C sort -u
)
if [[ -n "${missing_glyphs}" ]]; then
    echo "界面字体缺少汉字：${missing_glyphs}" >&2
    echo "请运行 ./scripts/generate-ui-font.sh 后重新检查" >&2
    exit 1
fi

echo "1/4 检查界面字体字形覆盖"
echo "字体覆盖检查通过"

echo "2/4 运行主机侧协议与隐私测试"
"${SCRIPT_DIR}/test-host.sh"

echo "3/4 编译完整固件"
zsh -lc 'source "$1" >/dev/null && idf.py -C "$2" build' _ "${DESK_IDF_ACTIVATE}" "${FIRMWARE_DIR}"

FIRMWARE_BIN="${FIRMWARE_DIR}/build/esp32_desk_console.bin"
if [[ ! -f "${FIRMWARE_BIN}" ]]; then
    echo "编译完成后未找到固件镜像" >&2
    exit 1
fi

echo "4/4 检查固件镜像"
wc -c "${FIRMWARE_BIN}"
echo "准备检查通过。本脚本不会连接、擦除或烧录开发板。"
