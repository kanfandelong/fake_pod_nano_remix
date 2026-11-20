idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES pod_display esp_lcd_co5300 esp_lcd_touch esp_lcd_touch_cst820 lvgl cs43131 pod_audio pod_debug pod_storage pod_font pod_ui driver esp_driver_i2c esp_driver_spi esp_pm
)
