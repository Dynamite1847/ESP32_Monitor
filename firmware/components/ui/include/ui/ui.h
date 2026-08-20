#pragma once

#include "app_model/app_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DESK_UI_ACTION_BACK = 1,
    DESK_UI_ACTION_FORWARD = 2,
    DESK_UI_ACTION_REFRESH = 3,
    DESK_UI_ACTION_NEW_TAB = 4,
    DESK_UI_ACTION_SCREENSHOT = 5,
    DESK_UI_ACTION_TERMINAL = 6,
    DESK_UI_ACTION_MEDIA_PREVIOUS = 7,
    DESK_UI_ACTION_MEDIA_PLAY_PAUSE = 8,
    DESK_UI_ACTION_MEDIA_NEXT = 9,
    DESK_UI_ACTION_MEDIA_MUTE = 10,
    DESK_UI_ACTION_MEDIA_VOLUME_DOWN = 11,
    DESK_UI_ACTION_MEDIA_VOLUME_UP = 12,
} desk_ui_action_id_t;

typedef void (*desk_ui_action_callback_t)(desk_ui_action_id_t action_id, void *context);

/** May be called before or after desk_ui_init. */
void desk_ui_set_action_callback(desk_ui_action_callback_t callback, void *context);

/** Caller must hold the esp_lv_adapter/LVGL lock. */
void desk_ui_init(const desk_app_state_t *initial_state);

/** Caller must hold the esp_lv_adapter/LVGL lock. */
void desk_ui_apply_state(const desk_app_state_t *state);

/** Caller must hold the esp_lv_adapter/LVGL lock. */
void desk_ui_show_home(void);

/**
 * 把已加载的 CJK 后备字体接到界面字体的 fallback，并触发重绘。
 * 字库已移出固件镜像、改由 SD 运行时加载（见 app_main 的后台字体任务，
 * 加载本身很慢且在锁外完成，这里只做快速的指针接线）。
 * 调用方必须持有 LVGL 锁。
 */
void desk_ui_set_cjk_fallback(lv_font_t *font);

#ifdef __cplusplus
}
#endif
