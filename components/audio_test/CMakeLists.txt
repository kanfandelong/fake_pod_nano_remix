idf_component_register(
    SRCS "audio_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_driver_gpio
)
