idf_component_register(
    SRCS "main.cpp" "qspi_clock_override.c"
    INCLUDE_DIRS ".")

# Force extraction from libmain.a even when the wrapped caller is linked later.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=spi_bus_add_device"
    "-Wl,--undefined=__wrap_spi_bus_add_device")

if(CONFIG_HOSINO_PERF_DEMO)
    target_sources(${COMPONENT_LIB} PRIVATE "hosino_perf_overlay.c")
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=lv_display_flush_ready"
        "-Wl,--undefined=__wrap_lv_display_flush_ready")
endif()

if(CONFIG_HOSINO_DISPLAY_BUFFERED)
    target_sources(${COMPONENT_LIB} PRIVATE "display_buffer_override.c")
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
