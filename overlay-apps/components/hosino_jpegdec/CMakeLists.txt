if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "hosino_jpegdec is pinned and tested for ESP32-S3 only")
endif()

idf_component_register(
    SRCS "hosino_jpegdec.c" "jpegdec_core.c"
         "vendor/JPEGDEC/src/s3_simd_420.S"
         "vendor/JPEGDEC/src/s3_simd_444.S"
         "vendor/JPEGDEC/src/s3_simd_dequant.S"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/JPEGDEC/src"
    REQUIRES esp_video_codec
    PRIV_REQUIRES heap log)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HOSINO_JPEGDEC_ESP32S3=1)
# Upstream portable decoder contains intentionally unused format helpers.
set_source_files_properties(jpegdec_core.c PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable")
