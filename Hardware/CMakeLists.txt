idf_component_register(
    SRCS
        "esp32/esp32_hardware.c"
        "esp32/ssd1306.c"
        "esp32/oled_font.c"
        "esp32/network_ota.c"
    INCLUDE_DIRS "include" "esp32"
    REQUIRES
        Application app_update app_trace esp_https_ota esp_http_server esp_wifi esp_event
        esp_netif nvs_flash esp_adc esp_driver_gpio esp_driver_ledc
        esp_driver_i2c esp_driver_pcnt esp_driver_uart esp_timer
        console json mbedtls lwip esp_http_client log esp_system freertos
)
