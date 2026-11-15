# Main inherits every component's public include directories. Only this
# directory needs response files to stay below Windows CreateProcess's limit;
# keep other component command lines unchanged for incremental builds.
if(CMAKE_HOST_WIN32 AND CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)
endif()

option(HOSINO_INSTALL_MEDIA_SAMPLES "Embed and install original SD test samples" OFF)

idf_component_register(
    SRCS "main.cpp" "hosino_bili_media.cpp" "qspi_clock_override.c" "hosino_sdcard_override.c" "hosino_lvgl_memory.c" "hosino_touch_override.c"
    INCLUDE_DIRS ".")

if(HOSINO_INSTALL_MEDIA_SAMPLES)
    target_sources(${COMPONENT_LIB} PRIVATE "hosino_media_samples.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HOSINO_INSTALL_MEDIA_SAMPLES=1)
    target_add_binary_data(${COMPONENT_LIB} "media_samples/generated-motion480.avi" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "media_samples/generated-tone.flac" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "media_samples/generated-tone24.flac" BINARY)
endif()

# Force extraction from libmain.a even when the wrapped caller is linked later.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=spi_bus_add_device"
    "-Wl,--undefined=__wrap_spi_bus_add_device")

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=bsp_sdcard_mount"
    "-Wl,--undefined=__wrap_bsp_sdcard_mount"
    "-Wl,--wrap=bsp_sdcard_unmount"
    "-Wl,--undefined=__wrap_bsp_sdcard_unmount")

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lv_malloc_core"
    "-Wl,--undefined=__wrap_lv_malloc_core"
    "-Wl,--wrap=lv_realloc_core"
    "-Wl,--undefined=__wrap_lv_realloc_core")

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_lcd_new_panel_io_i2c_v2"
    "-Wl,--undefined=__wrap_esp_lcd_new_panel_io_i2c_v2"
    "-Wl,--wrap=esp_lv_adapter_register_touch"
    "-Wl,--undefined=__wrap_esp_lv_adapter_register_touch"
    "-Wl,--wrap=esp_lv_adapter_unregister_touch"
    "-Wl,--undefined=__wrap_esp_lv_adapter_unregister_touch")

if(CONFIG_HOSINO_PERF_DEMO)
    target_sources(${COMPONENT_LIB} PRIVATE "hosino_perf_overlay.c")
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=lv_display_flush_ready"
        "-Wl,--undefined=__wrap_lv_display_flush_ready")
endif()

if(CONFIG_HOSINO_DISPLAY_BUFFERED)
    target_sources(${COMPONENT_LIB} PRIVATE "display_buffer_override.c" "hosino_lcd_staging.c")
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_lcd_panel_io_tx_color"
        "-Wl,--undefined=__wrap_esp_lcd_panel_io_tx_color")
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_lcd_new_panel_io_spi"
        "-Wl,--undefined=__wrap_esp_lcd_new_panel_io_spi"
        "-Wl,--wrap=esp_lv_adapter_register_display"
        "-Wl,--undefined=__wrap_esp_lv_adapter_register_display"
        "-Wl,--wrap=esp_lv_adapter_init"
        "-Wl,--undefined=__wrap_esp_lv_adapter_init")
endif()

if(CONFIG_HOSINO_DISPLAY_CORNERS)
    target_sources(${COMPONENT_LIB} PRIVATE "hosino_corner_clip.c")
endif()

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

# Function to get component library
function(get_component_library component_name output_var)
    # Get the exact component name
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT STREQUAL "${component_name}" OR COMPONENT MATCHES "^[^/]+__${component_name}$")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    # Get the component library
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

get_component_library("lvgl" LVGL_LIB)
target_compile_options(
    ${LVGL_LIB}
    PUBLIC
        "-Wno-attributes"
)
