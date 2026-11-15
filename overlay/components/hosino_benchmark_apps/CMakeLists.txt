idf_component_register(
    SRCS "hosino_benchmark_apps.cpp" "hosino_corner_cal.cpp"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core esp_lvgl_adapter
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
