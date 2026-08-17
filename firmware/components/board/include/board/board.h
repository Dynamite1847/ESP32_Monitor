#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_LCD_H_RES 800
#define DESK_LCD_V_RES 480

typedef enum {
    DESK_BOARD_PROFILE_4_3 = 0,
    DESK_BOARD_PROFILE_4_3_B,
} desk_board_profile_id_t;

typedef struct {
    desk_board_profile_id_t id;
    const char *display_name;
    bool hardware_verified;
} desk_board_profile_t;

const desk_board_profile_t *desk_board_get_profile(void);

esp_err_t desk_board_display_init(
    uint8_t frame_buffer_count,
    esp_lcd_panel_handle_t *panel_handle,
    esp_lcd_touch_handle_t *touch_handle
);

esp_err_t desk_board_backlight_set(bool enabled);

#ifdef __cplusplus
}
#endif
