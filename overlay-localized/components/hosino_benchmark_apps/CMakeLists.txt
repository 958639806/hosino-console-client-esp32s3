idf_component_register(
    SRCS "hosino_benchmark_apps.cpp"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_ui_fonts
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
