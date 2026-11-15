set(bench_sources "hosino_lvgl_benchmark.cpp")
if(CONFIG_HOSINO_DISPLAY_BUFFERED)
    list(APPEND bench_sources "portengine.c"
        "assets/img_benchmark_avatar.c"
        "assets/img_benchmark_lvgl_logo_argb.c"
        "assets/img_benchmark_lvgl_logo_rgb.c")
endif()
idf_component_register(
    SRCS ${bench_sources}
    INCLUDE_DIRS "."
    REQUIRES brookesia_core
    WHOLE_ARCHIVE
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
if(CONFIG_HOSINO_DISPLAY_BUFFERED)
    # Private to this derivative and its copied assets, never the LVGL library.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        LV_USE_DEMO_BENCHMARK=1 LV_DEMO_BENCHMARK_ALIGNED_FONTS=0)
endif()
