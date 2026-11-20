idf_component_register(
    SRCS "cs43131.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio
)
