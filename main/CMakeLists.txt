idf_component_register(
    SRCS "asset_manager.c" "main.c" "mobilenet_wrapper.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp32-camera 
        nvs_flash 
        esp_timer
        imagenet_cls
        fatfs
        sdmmc
)
