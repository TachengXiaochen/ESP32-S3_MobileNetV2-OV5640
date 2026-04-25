#include "storage_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <sys/stat.h>

static const char *TAG = "storage_mod";
static bool g_is_initialized = false;

bool storage_module_init(void) {
    if (g_is_initialized) return true;

    ESP_LOGI(TAG, "Initializing SD card and file system...");
    esp_err_t ret = asset_manager_init();
    if (ret == ESP_OK) {
        g_is_initialized = true;
        return true;
    }
    return false;
}

save_result_t storage_module_save_asset(const asset_record_t *record) {
    if (!g_is_initialized) return SAVE_RESULT_FAILED;
    
    // 增加延迟和看门狗复位，确保稳定性
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_task_wdt_reset();
    
    bool is_overwrite = false;
    esp_err_t ret = asset_save(record, &is_overwrite);
    
    if (ret != ESP_OK) {
        return SAVE_RESULT_FAILED;
    }
    
    return is_overwrite ? SAVE_RESULT_SUCCESS_OVERWRITE : SAVE_RESULT_SUCCESS_NEW;
}