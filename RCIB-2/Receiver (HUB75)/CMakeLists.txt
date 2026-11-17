idf_component_register(
    SRCS "RCIB-Radio-RX.cpp"
    INCLUDE_DIRS "."
    REQUIRES hub75 esp_wifi esp_event esp_netif nvs_flash
)
