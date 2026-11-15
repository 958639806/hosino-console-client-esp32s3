idf_component_register(
    SRCS "HosinoConsole.cpp" "hosino_console_icon.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_console_client hosino_ui_fonts hosino_chat_fonts
             hosino_buttons nvs_flash esp_timer heap freertos
    WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
