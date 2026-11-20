idf_component_register(
    SRCS "pod_debug.c"
    INCLUDE_DIRS "include"
    REQUIRES console esp_driver_i2c esp_partition esp_system esp_hw_support spi_flash cs43131 pod_ui
)
