idf_component_register(
    SRCS "hosino_chat_font_16.c" "hosino_chat_symbols_16.c" "hosino_chat_display.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
