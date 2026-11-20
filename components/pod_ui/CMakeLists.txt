idf_component_register(
    SRCS "pod_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl pod_storage pod_audio pod_metadata pod_font pod_display esp_lcd espressif__esp_lcd_co5300 esp_driver_gpio mbedtls
)
