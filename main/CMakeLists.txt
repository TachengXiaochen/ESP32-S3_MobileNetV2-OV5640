idf_component_register(
    SRCS "asset_manager.c" "main.c" "camera_stream.c" "mobilenet_wrapper.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        esp32-camera 
        esp_http_server 
        esp_netif 
        esp_wifi 
        nvs_flash 
        esp_timer 
        esp_event
        imagenet_cls
        fatfs
        sdmmc
        spiffs
)
