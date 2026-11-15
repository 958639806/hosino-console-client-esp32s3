idf_component_register(
    SRCS "hosino_font_14.c" "hosino_font_16.c" "hosino_font_18.c" "hosino_font_20.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
