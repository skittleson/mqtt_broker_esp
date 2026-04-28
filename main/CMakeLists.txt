idf_component_register(
    SRCS "main.c" "wifi_connect.c" "portal.c" "mqtt_parser.c" "mqtt_broker.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_event app_update esp_http_client spi_flash
)
