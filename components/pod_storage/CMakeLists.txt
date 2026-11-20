idf_component_register(
    SRCS "pod_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc nvs_flash joltwallet__littlefs vfs esp_driver_gpio
)
