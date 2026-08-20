#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
FONT_DIR="${FIRMWARE_DIR}/.cache/fonts"
FONT_PATH="${FONT_DIR}/SourceHanSansSC-Regular.otf"
FONT_URL="https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
FONT_SHA256="f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256"
UI_OUTPUT_PATH="${FIRMWARE_DIR}/components/ui/fonts/desk_ui_font_16.c"
CJK_OUTPUT_PATH="${FIRMWARE_DIR}/assets/desk_ui_cjk_font_16.bin"
CJK_RANGES="0x00A0-0x024F,0x0370-0x052F,0x1100-0x11FF,0x2000-0x206F,0x2100-0x214F,0x2190-0x22FF,0x2460-0x26FF,0x2E80-0x33FF,0x3400-0x4DBF,0x4E00-0x9FFF,0xAC00-0xD7AF,0xF900-0xFAFF,0xFE10-0xFE4F,0xFF00-0xFFEF"

mkdir -p "${FONT_DIR}"
if [[ ! -f "${FONT_PATH}" ]]; then
    echo "下载思源黑体简体中文常规字重"
    curl -L --fail --retry 3 "${FONT_URL}" -o "${FONT_PATH}"
fi

if command -v shasum >/dev/null 2>&1; then
    actual_sha256="$(shasum -a 256 "${FONT_PATH}" | awk '{print $1}')"
else
    actual_sha256="$(sha256sum "${FONT_PATH}" | awk '{print $1}')"
fi
if [[ "${actual_sha256}" != "${FONT_SHA256}" ]]; then
    echo "字体文件校验失败：${actual_sha256}" >&2
    exit 1
fi

han_glyphs="$(
    while IFS= read -r source_file; do
        rg --no-filename -oP '[\x{3400}-\x{9FFF}]' "${source_file}" || true
    done < <(
        rg --files \
            "${FIRMWARE_DIR}/components/ui" \
            "${FIRMWARE_DIR}/components/app_model" \
            -g '*.c' -g '*.h' -g '*.txt' \
            -g '!desk_ui_font_16.c' -g '!desk_ui_cjk_font_16.c'
    ) |
        LC_ALL=C sort -u |
        tr -d '\n'
)"
symbols="${han_glyphs}°·，（）～"

cd "${FIRMWARE_DIR}"
mkdir -p assets
# CJK 后备字库输出为 LVGL 二进制字体（.bin），部署到 SD，不再编进固件镜像。
npx --yes lv_font_conv@1.5.3 \
    --font ".cache/fonts/SourceHanSansSC-Regular.otf" \
    -r "${CJK_RANGES}" \
    --size 16 \
    --bpp 2 \
    --format bin \
    --no-kerning \
    -o "assets/desk_ui_cjk_font_16.bin"

# 固定界面字体：不再编译期绑定 fallback（改为运行时从 SD 接入）。
npx --yes lv_font_conv@1.5.3 \
    --font ".cache/fonts/SourceHanSansSC-Regular.otf" \
    -r 0x20-0x7E \
    --symbols "${symbols}" \
    --size 16 \
    --bpp 4 \
    --format lvgl \
    --no-compress \
    --no-prefilter \
    --force-fast-kern-format \
    --lv-include "lvgl.h" \
    --lv-font-name desk_ui_font_16 \
    -o "components/ui/fonts/desk_ui_font_16.c"

# 改成非 const，使运行时可写入 .fallback（LVGL 9 默认生成 const）。
sed -i '' 's/^const lv_font_t desk_ui_font_16 = {/lv_font_t desk_ui_font_16 = {/' "${UI_OUTPUT_PATH}"

echo "已生成 ${CJK_OUTPUT_PATH}，覆盖完整东亚常用字符区（部署到 SD）。"
echo "已生成 ${UI_OUTPUT_PATH}，包含 $(printf '%s' "${han_glyphs}" | wc -m | tr -d ' ') 个高质量界面汉字。"
