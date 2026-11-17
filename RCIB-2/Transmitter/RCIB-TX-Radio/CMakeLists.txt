idf_component_register(
    SRCS "RCIB-TX.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_uart
        esp_wifi
        nvs_flash
)
