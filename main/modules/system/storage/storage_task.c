/**
 * @file storage_task.c
 * @brief 存储管理任务 — 处理 TF 卡初始化
 *
 * 从 main.c 拆分。接收 xStorageQueue 中的存储命令。
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"

#include "storage_task.h"
#include "main.h"
#include "storage_module.h"

static const char *TAG = "camera_ai";
#define SAFE_WDT_RESET()    esp_task_wdt_reset()

// ========== 存储管理任务 ==========
void storage_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    system_msg_t msg;
    while (1) {
        if (xQueueReceive(xStorageQueue, &msg, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            switch (msg.cmd) {
                case CMD_INIT_STORAGE:
                    if (storage_module_init()) { g_storage_ready = true; ESP_LOGI(TAG, "Storage OK"); }
                    else ESP_LOGE(TAG, "Storage FAIL");
                    break;
                default: ESP_LOGW(TAG, "Unknown storage cmd %d", msg.cmd); break;
            }
        } else { SAFE_WDT_RESET(); }
    }
}
