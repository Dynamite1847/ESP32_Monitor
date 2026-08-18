idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES app_auth app_model ble_link board esp_app_format http_fetch json market network nvs_flash privacy storage ui weather esp_lvgl_adapter
)
