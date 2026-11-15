idf_component_register(
    SRCS "hosino_gravity_app.cpp" "assets/img_app_qmi8658ball.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_ui_fonts waveshare__esp32_s3_touch_amoled_2_16
             waveshare__qmi8658 esp_driver_i2c
    WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
