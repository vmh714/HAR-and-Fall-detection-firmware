idf_component_register(
    SRCS "drv_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_timer
)
