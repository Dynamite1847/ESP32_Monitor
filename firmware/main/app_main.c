#include <assert.h>

#include "app_model/app_model.h"
#include "board/board.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "privacy/privacy_state_machine.h"
#include "ui/ui.h"

static const char *TAG = "desk_console";

void app_main(void)
{
    const desk_board_profile_t *profile = desk_board_get_profile();
    ESP_LOGI(TAG, "Board profile: %s", profile->display_name);

    desk_app_state_t app_state;
    desk_app_state_load_mock(&app_state);

    desk_privacy_context_t privacy_context;
    desk_privacy_init(&privacy_context, 6000);
    ESP_LOGI(TAG, "Privacy state initialized: %d", privacy_context.state);

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(desk_board_display_init(frame_buffer_count, &panel_handle, &touch_handle));

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        DESK_LCD_H_RES,
        DESK_LCD_V_RES,
        rotation
    );
    display_config.profile.use_psram = true;

    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    assert(display != NULL);

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        desk_ui_init(&app_state);
        esp_lv_adapter_unlock();
    }

#if CONFIG_DESK_BRINGUP_DISPLAY_ON
    ESP_LOGW(TAG, "Bring-up mode is active: backlight stays on without BLE authentication");
    ESP_ERROR_CHECK(desk_board_backlight_set(true));
#else
    ESP_ERROR_CHECK(desk_board_backlight_set(false));
#endif
}
