idf_component_register(SRCS "hosino_console_auth_store.c" INCLUDE_DIRS "include"
    REQUIRES esp_common PRIV_REQUIRES nvs_flash freertos heap)
