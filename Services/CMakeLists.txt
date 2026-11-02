idf_component_register(
    SRCS
        "src/fan_service.c"
        "src/battery_service.c"
        "src/ui_renderer.c"
    INCLUDE_DIRS "include"
    REQUIRES Application Hardware
)
