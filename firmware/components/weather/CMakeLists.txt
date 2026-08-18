idf_component_register(
    SRCS "weather.c" "qweather_parser.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES app_model http_fetch json nvs_flash
)
