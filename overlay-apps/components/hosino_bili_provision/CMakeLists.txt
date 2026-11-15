idf_component_register(SRCS "hosino_bili_provision.c" "provision_parser.c" "provision_store.c" "config_store.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_common nvs_flash heap freertos gmf_io vfs esp_driver_usb_serial_jtag log)
