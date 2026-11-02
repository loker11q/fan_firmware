idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        Application Services Hardware esp_event esp_timer esp_system esp_hw_support nvs_flash
        app_update esp_app_format log freertos
)
