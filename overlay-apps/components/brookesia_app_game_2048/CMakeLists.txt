idf_component_register(
    SRCS "esp_brookesia_app_game_2048.cpp" "assets/img_app_2048.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_ui_fonts
    WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
