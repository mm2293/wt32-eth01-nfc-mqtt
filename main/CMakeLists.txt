idf_component_register(
    SRCS "main.c" "ethernet_setup.c" "pn532_uart.c" "relay_control.c" "mqtt_client_setup.c" "keypad.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif driver mqtt json nvs_flash esp_event esp_timer
)
