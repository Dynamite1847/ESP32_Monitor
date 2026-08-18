idf_component_register(
    SRCS "storage.c" "log_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES board fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_timer esp_system
)
