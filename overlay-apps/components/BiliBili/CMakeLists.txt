idf_component_register(SRCS "BiliBili.cpp" "BiliClient.cpp" "BiliDns.c" "bili_icon.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core hosino_media_player hosino_ui_fonts hosino_chat_fonts
             esp_http_client esp_netif esp-tls esp_wifi lwip mbedtls json esp_timer heap freertos gmf_io hosino_bili_provision
    WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
