idf_component_register(
    SRCS "pod_display.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_lcd espressif__esp_lcd_touch esp_driver_gpio esp_timer nvs_flash
)
