#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 非 const：CJK 后备字库从 SD 运行时加载后写入 .fallback。 */
extern lv_font_t desk_ui_font_16;

#ifdef __cplusplus
}
#endif
