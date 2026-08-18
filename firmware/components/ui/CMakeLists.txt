idf_component_register(
    SRCS
        "ui.c"
        "fonts/desk_ui_font_16.c"
        "fonts/desk_ui_cjk_font_16.c"
    INCLUDE_DIRS "include"
    REQUIRES app_model lvgl
)
