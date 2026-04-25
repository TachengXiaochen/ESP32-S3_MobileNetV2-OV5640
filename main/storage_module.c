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

bool storage_module_save_asset(const asset_record_t *record) {
    if (!g_is_initialized) return false;
    
    // 增加延迟和看门狗复位，确保稳定性
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_task_wdt_reset();
    
    esp_err_t ret = asset_save(record);
    return (ret == ESP_OK);
}
