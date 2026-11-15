idf_component_register(
    SRCS "esp_brookesia_app_timer.cpp" "assets/img_app_timer.c" "assets/ui_font_Number_big.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_ui_fonts
    WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
