idf_component_register(
    SRCS "app_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support mbedtls nvs_flash
)
