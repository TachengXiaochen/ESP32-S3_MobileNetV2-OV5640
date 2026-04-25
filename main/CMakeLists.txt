idf_component_register(SRCS "main.c" "asset_manager.c" "camera_module.c" "storage_module.c" "ai_module.c" "mobilenet_wrapper.cpp" "cmd_handler.c" "led_indicator.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp-dl imagenet_cls fatfs sdmmc nvs_flash driver)
