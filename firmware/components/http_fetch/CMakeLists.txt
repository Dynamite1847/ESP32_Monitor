idf_component_register(
    SRCS "http_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_rom esp_http_client mbedtls
)
