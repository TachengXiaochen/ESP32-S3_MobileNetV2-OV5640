/**
 * @file main.c
 * @brief ESP32-S3 CAM AI 应用主入口
 *
 * 职责（精简后，~100行）：
 *   1. 全局变量定义（IPC 句柄 + 运行时状态 + 特征缓冲区）
 *   2. 模块初始化编排（app_main）
 *   3. FreeRTOS 任务创建
 *
 * 命令处理已移至 app_handlers.c
 * 推理任务已移至 modules/ai/inference_task.c
 * 存储任务已移至 modules/system/storage/storage_task.c
 * UART0 初始化/系统信息已移至 modules/system/comm/uart_handler_0.c
 */

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "main.h"
#include "app_handlers.h"
#include "modules/ai/inference_task.h"
#include "modules/system/storage/storage_task.h"
#include "modules/camera/camera_module.h"
#include "modules/system/storage/storage_module.h"
#include "modules/ai/ai_module.h"
#include "modules/system/led/led_indicator.h"
#include "modules/system/verify/verify_handler.h"
#include "modules/system/comm/uart_handler_0.h"
#include "modules/system/comm/uart_handler_1.h"
#include "modules/web/web_server.h"
#include "modules/4g/l610_manager.h"

static const char *TAG = "camera_ai";

// ===================================================================
//  全局变量定义（精简：31个独立变量 → 1个上下文结构体 + 4个IPC + 6个特征数组 + 1个调试变量）
// ===================================================================

// --- 运行时上下文（20个字段合并为1个结构体）---
app_context_t g_ctx = {0};

// --- IPC 句柄 ---
QueueHandle_t xSystemQueue = NULL;
QueueHandle_t xStorageQueue = NULL;
QueueHandle_t xInferenceQueue = NULL;
SemaphoreHandle_t xCameraMutex = NULL;

// --- 特征缓冲区（6 × 1280 floats = 30KB，PSRAM 对齐）---
float g_front_feature[FEATURE_VEC_SIZE] = {0};
float g_side_feature[FEATURE_VEC_SIZE] = {0};
float g_top_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_front_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_side_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_top_feature[FEATURE_VEC_SIZE] = {0};

// --- UART0 调试专用（verify_handler 模块直接引用）---
verify_context_t g_verify_ctx = {0};

// ===================================================================
//  主程序入口
// ===================================================================
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_uart();
    led_indicator_init();
    led_set_color(255, 0, 0);

    // 创建 IPC 对象
    xSystemQueue = xQueueCreate(10, sizeof(system_msg_t));
    xStorageQueue = xQueueCreate(5, sizeof(system_msg_t));
    xInferenceQueue = xQueueCreate(5, sizeof(inference_job_t));
    xCameraMutex = xSemaphoreCreateMutex();

    // 主菜单（调试用）
    printf("\n========== MAIN MENU ==========\r\n");
    printf("  r - Register\r\n  o - Outbound\r\n  c - Inventory\r\n  d - Delete\r\n  l - List\r\n  i - Info\r\n  help/? - Menu\r\n===============================\r\n");
    printf("[GUIDE] Select: "); fflush(stdout);
    ESP_LOGI(TAG, "System Ready");

    // 存储 & AI 初始化
    storage_module_init();
    ai_module_init();
    be_init(be_output_callback);

    // 摄像头在 app_main 中一次性初始化（调度器启动前，core 0 上下文）
    // i2c_driver_install 必须在 app_main 上下文调用，不能从 FreeRTOS 任务调用
    // 摄像头硬件保持初始化状态，电源管理通过 GPIO48 单独控制
    if (camera_module_init()) {
        ESP_LOGI(TAG, "Camera initialized at boot");
    } else {
        ESP_LOGE(TAG, "Camera init failed at boot");
    }

    // 先在干净的中断环境下创建任务，再初始化 UART（避免 ISR 干扰任务调度）
    xTaskCreate(camera_ai_task, "camera_ai_task", 16384, NULL, 7, NULL);
    xTaskCreate(inference_task, "inference_task", 8192, NULL, 4, NULL);
    xTaskCreate(storage_task, "storage_task", 8192, NULL, 4, NULL);

    // 任务创建完毕后才启用 UART（UART ISR 不再抢占）
    uart_handler_0_init();
    uart_handler_1_init();

    // 延迟3秒确保SD卡DMA、PSRAM完全释放后再启动WiFi
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Starting web server (delayed init)...");
    web_server_init();

    // WiFi 就绪后再初始化 L610（避免L610 AT超时阻塞Web预览）
    ret = l610_manager_init();
    if (ret == ESP_OK) {
        ret = l610_manager_start();
        if (ret == ESP_OK) ESP_LOGI(TAG, "L610 OK");
        else ESP_LOGW(TAG, "L610 start fail");
    } else ESP_LOGW(TAG, "L610 init fail");

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
