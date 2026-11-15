idf_component_register(SRCS "hosino_console_client.cpp" "console_protocol.c" "console_voice_wav.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_timer freertos heap log lwip bsp_extra hosino_console_auth_store)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
