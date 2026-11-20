idf_component_register(
    SRCS "pod_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES espaudiocore cs43131 esp_driver_i2s esp_driver_gpio
)
