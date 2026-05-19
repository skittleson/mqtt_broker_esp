idf_component_register(
    SRCS "main.c" "wifi_connect.c" "portal.c" "portal_ws.c" "mqtt_parser.c" "mqtt_broker.c" "eth_connect.c" "ntp.c" "csrf.c" "timers.c" "tz_presets.c" "berry_runtime.c" "portal_berry.c" "berry_mod_mqtt.c" "berry_mod_http.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_eth nvs_flash esp_event app_update esp_http_client spi_flash driver mbedtls esp_timer lwip berry
)
