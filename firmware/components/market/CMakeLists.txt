idf_component_register(
    SRCS "market.c" "market_parser.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES app_model http_fetch json
)
