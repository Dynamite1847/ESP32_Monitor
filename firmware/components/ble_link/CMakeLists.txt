idf_component_register(
    SRCS "ble_link.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash ble_protocol
)
