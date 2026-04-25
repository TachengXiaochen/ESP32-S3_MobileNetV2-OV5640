#include "esp_system.h"

// 通过串口输出所有资产列表
void asset_list_uart(void) {
    int count = 0;
    extern esp_err_t asset_list_all(int *count);
    char buf[128];
    uart_write_bytes(UART_NUM, (const char *)"\r\n[ASSET LIST]\r\n", 16);
    esp_err_t ret = asset_list_all(&count);
    if (ret == ESP_OK) {
        snprintf(buf, sizeof(buf), "Total: %d assets\r\n", count);
        uart_write_bytes(UART_NUM, buf, strlen(buf));
    } else {
        uart_write_bytes(UART_NUM, (const char *)"Failed to list assets or storage not initialized.\r\n", 51);
    }
}

// 通过串口输出系统信息
void print_system_info_uart(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Free heap: %u bytes\r\n", (unsigned)esp_get_free_heap_size());
    uart_write_bytes(UART_NUM, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Min free heap: %u bytes\r\n", (unsigned)esp_get_minimum_free_heap_size());
    uart_write_bytes(UART_NUM, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "SDK version: %s\r\n", esp_get_idf_version());
    uart_write_bytes(UART_NUM, buf, strlen(buf));
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>    // stat, mkdir
#include <dirent.h>      // opendir, readdir
#include <errno.h>       // errno
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

// Modular interfaces
#include "camera_module.h"
#include "storage_module.h"
#include "ai_module.h"
#include "asset_manager.h" // For asset_record_t and MAC_ADDR_LEN if defined there

// ========== FreeRTOS 任务与队列定义 ==========
#define UART_QUEUE_LEN    10
#define STORAGE_QUEUE_LEN 5

// 摄像头状态枚举
typedef enum {
    CAM_STATE_WAITING_MAC = 0,
    CAM_STATE_READY = 1
} camera_state_t;

// 视图状态枚举
typedef enum {
    VIEW_NONE = 0,
    VIEW_FRONT = 1,
    VIEW_SIDE = 2,
    VIEW_TOP = 3
} view_state_t;

typedef enum {
    CMD_INIT_CAMERA,
    CMD_CAPTURE_FRONT,
    CMD_CAPTURE_SIDE,
    CMD_CAPTURE_TOP,
    CMD_SAVE_ASSET,
    CMD_INIT_STORAGE,
    CMD_START_INVENTORY  // 新增：启动盘点模式
} system_cmd_t;

typedef struct {
    system_cmd_t cmd;
    void *data;        // 用于传递特征向量指针等
    char mac[MAC_ADDR_LEN + 1];
} system_msg_t;

static QueueHandle_t xSystemQueue = NULL;
static QueueHandle_t xStorageQueue = NULL;

// 看门狗函数声明（如果头文件中未定义）
esp_err_t esp_task_wdt_add(TaskHandle_t handle);
esp_err_t esp_task_wdt_reset(void);

// 看门狗安全包装宏
#define SAFE_WDT_ADD()      esp_task_wdt_add(NULL)
#define SAFE_WDT_RESET()    esp_task_wdt_reset()

// 定义MAX和MIN宏
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// UART 配置
#define UART_NUM UART_NUM_0
#define UART_BAUD_RATE 115200
#define UART_BUF_SIZE (1024 * 2)

// 特征向量维度
#define FEATURE_VEC_SIZE 1280

static const char *TAG = "camera_ai";

// Global state variables
static bool g_camera_ready = false;
static bool g_storage_ready = false;

// Shared state for multi-view capture
float g_front_feature[FEATURE_VEC_SIZE] = {0};
float g_side_feature[FEATURE_VEC_SIZE] = {0};
float g_top_feature[FEATURE_VEC_SIZE] = {0};

// 盘点模式状态
typedef enum {
    INVENTORY_IDLE = 0,
    INVENTORY_WAITING_FRONT,   // 等待正面拍摄
    INVENTORY_WAITING_SIDE,    // 等待侧面拍摄
    INVENTORY_WAITING_TOP,     // 等待顶部拍摄
    INVENTORY_ANALYZING        // 分析阶段
} inventory_state_t;

static inventory_state_t g_inventory_state = INVENTORY_IDLE;
static float g_inventory_confidence[3] = {0}; // 三个视图的置信度

char g_current_mac[MAC_ADDR_LEN + 1] = {0};
camera_state_t g_camera_state = CAM_STATE_WAITING_MAC;
view_state_t g_view_state = VIEW_NONE;
bool g_camera_power_on = false;
bool g_storage_initialized = false;

static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);
    
    ESP_LOGI(TAG, "UART initialized at %d baud", UART_BAUD_RATE);
}

// 验证MAC地址格式
static bool validate_mac_address(const char *mac)
{
    if (strlen(mac) != MAC_ADDR_LEN) {
        return false;
    }
    
    // 格式: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < MAC_ADDR_LEN; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (mac[i] != ':') {
                return false;
            }
        } else {
            if (!((mac[i] >= '0' && mac[i] <= '9') || 
                  (mac[i] >= 'A' && mac[i] <= 'F') ||
                  (mac[i] >= 'a' && mac[i] <= 'f'))) {
                return false;
            }
        }
    }
    return true;
}

// 处理串口命令 - 仅负责发送指令到队列
static void handle_uart_command(const char *cmd_str)
{
    // 去除末尾换行符
    char cmd[128] = {0};
    strncpy(cmd, cmd_str, sizeof(cmd) - 1);
    int len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r')) {
        cmd[--len] = '\0';
    }
    
    // 存储模式切换命令
    if (strcasecmp(cmd, "storage sd") == 0) {
        uart_write_bytes(UART_NUM, (const char *)"Switching to SD Card mode...\r\n", 33);
        esp_err_t ret = asset_switch_storage_mode(STORAGE_MODE_SD_CARD);
        if (ret == ESP_OK) {
            uart_write_bytes(UART_NUM, (const char *)"Storage switched to SD Card\r\n", 32);
        } else {
            uart_write_bytes(UART_NUM, (const char *)"Failed to switch to SD Card mode\r\n", 36);
        }
        return;
    } else if (strcasecmp(cmd, "storage flash") == 0) {
        uart_write_bytes(UART_NUM, (const char *)"Switching to SPIFFS (Flash) mode...\r\n", 40);
        esp_err_t ret = asset_switch_storage_mode(STORAGE_MODE_SPIFFS);
        if (ret == ESP_OK) {
            uart_write_bytes(UART_NUM, (const char *)"Storage switched to SPIFFS (Internal Flash)\r\n", 47);
        } else {
            uart_write_bytes(UART_NUM, (const char *)"Failed to switch to SPIFFS mode\r\n", 35);
        }
        return;
    } else if (strcasecmp(cmd, "storage status") == 0) {
        storage_mode_t mode = asset_get_storage_mode();
        char msg[128];
        snprintf(msg, sizeof(msg), "Current storage mode: %s\r\n", 
                 mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS (Internal Flash)");
        uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
        return;
    } else if (strcasecmp(cmd, "l") == 0 || strcasecmp(cmd, "list") == 0) {
        // 列出所有资产
        extern void asset_list_uart(void); // 需在asset_manager.c实现
        asset_list_uart();
        return;
    } else if (strcasecmp(cmd, "i") == 0 || strcasecmp(cmd, "info") == 0) {
        // 显示系统信息
        extern void print_system_info_uart(void); // 需在main.c或其他文件实现
        print_system_info_uart();
        return;
    } else if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        // 显示帮助
        uart_write_bytes(UART_NUM, (const char *)"\r\n[HELP] Command List:\r\n", 26);
        uart_write_bytes(UART_NUM, (const char *)"  MAC地址: AA:BB:CC:DD:EE:FF\r\n", 32);
        uart_write_bytes(UART_NUM, (const char *)"  f/s/t: 拍摄正/侧/顶视图\r\n", 30);
        uart_write_bytes(UART_NUM, (const char *)"  c: 进入盘点引导模式\r\n", 26);
        uart_write_bytes(UART_NUM, (const char *)"  l: 列出所有资产\r\n", 20);
        uart_write_bytes(UART_NUM, (const char *)"  i: 显示系统信息\r\n", 20);
        uart_write_bytes(UART_NUM, (const char *)"  storage sd/flash/status: 存储相关\r\n", 38);
        uart_write_bytes(UART_NUM, (const char *)"  help/?: 显示本帮助\r\n", 22);
        return;
    }
    
    // MAC地址输入（仅在等待MAC状态时接受）
    if (g_camera_state == CAM_STATE_WAITING_MAC) {
        if (validate_mac_address(cmd)) {
            strncpy(g_current_mac, cmd, MAC_ADDR_LEN);
            
            // 1. 发送初始化存储指令
            system_msg_t init_storage_msg = {0};
            init_storage_msg.cmd = CMD_INIT_STORAGE;
            xQueueSend(xStorageQueue, &init_storage_msg, portMAX_DELAY);
            
            // 2. 发送初始化摄像头指令
            system_msg_t init_cam_msg = {0};
            init_cam_msg.cmd = CMD_INIT_CAMERA;
            xQueueSend(xSystemQueue, &init_cam_msg, portMAX_DELAY);
            
            g_camera_state = CAM_STATE_READY;
            uart_write_bytes(UART_NUM, (const char *)"\r\n[SYSTEM] Hardware initialized.\r\n", 34);
            uart_write_bytes(UART_NUM, (const char *)"[GUIDE] Please input 'f' (Front), 's' (Side), or 't' (Top) to capture.\r\n", 69);
        } else {
            uart_write_bytes(UART_NUM, (const char *)"Invalid MAC address format. Example: AA:BB:CC:DD:EE:FF\r\n", 58);
        }
        return;
    }

    // 拍摄命令通过队列发送
    if (g_camera_state == CAM_STATE_READY) {
        system_msg_t msg = {0}; // 初始化为0确保所有字段干净
        strncpy(msg.mac, g_current_mac, MAC_ADDR_LEN);
        msg.mac[MAC_ADDR_LEN] = '\0'; // 显式确保字符串终止
        
        // 检查是否处于盘点模式
        if (g_inventory_state != INVENTORY_IDLE) {
            // 盘点模式下的引导式拍摄
            switch (cmd[0]) {
                case 'f':
                case 'F':
                    if (g_inventory_state == INVENTORY_WAITING_FRONT) {
                        msg.cmd = CMD_CAPTURE_FRONT;
                        xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"\r\n[INVENTORY] Please capture FRONT view first\r\n", 46);
                    }
                    break;
                    
                case 's':
                case 'S':
                    if (g_inventory_state == INVENTORY_WAITING_SIDE) {
                        msg.cmd = CMD_CAPTURE_SIDE;
                        xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"\r\n[INVENTORY] Please capture SIDE view now\r\n", 42);
                    }
                    break;
                    
                case 't':
                case 'T':
                    if (g_inventory_state == INVENTORY_WAITING_TOP) {
                        msg.cmd = CMD_CAPTURE_TOP;
                        xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"\r\n[INVENTORY] Please capture TOP view last\r\n", 43);
                    }
                    break;
                    
                default:
                    uart_write_bytes(UART_NUM, (const char *)"\r\n[INVENTORY] Follow the guide: f -> s -> t\r\n", 44);
                    break;
            }
        } else {
            // 普通模式下的单视图拍摄
            switch (cmd[0]) {
                case 'f': 
                    msg.cmd = CMD_CAPTURE_FRONT; 
                    xQueueSend(xSystemQueue, &msg, portMAX_DELAY); 
                    break;
                case 's': 
                    msg.cmd = CMD_CAPTURE_SIDE; 
                    xQueueSend(xSystemQueue, &msg, portMAX_DELAY); 
                    break;
                case 't': 
                    msg.cmd = CMD_CAPTURE_TOP; 
                    xQueueSend(xSystemQueue, &msg, portMAX_DELAY); 
                    break;
                case 'c':
                case 'C':
                    msg.cmd = CMD_START_INVENTORY;
                    xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                    break;
            }
        }
    }
}

// UART 接收任务 - 修改为按行读取
static void uart_task(void *pvParameters)
{
    // 注册当前任务到看门狗监控列表
    esp_task_wdt_add(NULL);
    
    uint8_t *data = (uint8_t *) malloc(UART_BUF_SIZE);
    char line_buf[128] = {0};
    int line_pos = 0;
    
    while (1) {
        int len = uart_read_bytes(UART_NUM, data, UART_BUF_SIZE, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t ch = data[i];
                
                // 检测回车或换行符，表示一行结束
                if (ch == '\r' || ch == '\n') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received command: %s", line_buf); // 调试日志
                        handle_uart_command(line_buf);
                        line_pos = 0;
                        memset(line_buf, 0, sizeof(line_buf));
                    }
                } else {
                    // 普通字符，添加到缓冲区
                    if (line_pos < sizeof(line_buf) - 1) {
                        line_buf[line_pos++] = ch;
                    }
                }
            }
        }
        
        // 定期复位看门狗，防止长耗时操作导致重启
        SAFE_WDT_RESET();
    }
    
    free(data);
    vTaskDelete(NULL);
}

// ========== AI任务：独立处理摄像头和推理 ==========
static void camera_ai_task(void *pvParameters)
{
    // 注册当前任务到看门狗监控列表
    esp_task_wdt_add(NULL);
    
    system_msg_t msg;
    
    while (1) {
        // 使用超时接收，防止任务长期阻塞导致看门狗触发
        if (xQueueReceive(xSystemQueue, &msg, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            
            switch (msg.cmd) {
                case CMD_INIT_CAMERA:
                    if (camera_module_init()) {
                        g_camera_ready = true;
                        g_camera_power_on = true;
                        uart_write_bytes(UART_NUM, (const char *)"Camera powered ON\r\n", 20);
                    }
                    break;

                case CMD_CAPTURE_FRONT:
                case CMD_CAPTURE_SIDE:
                case CMD_CAPTURE_TOP:
                    if (!g_camera_ready) {
                        uart_write_bytes(UART_NUM, (const char *)"Camera not ready!\r\n", 20);
                        break;
                    }
                    
                    // 在开始特征提取前喂狗
                    SAFE_WDT_RESET();
                    
                    float *feature_vec = (float *)malloc(FEATURE_VEC_SIZE * sizeof(float));
                    if (feature_vec && camera_module_capture_and_process(feature_vec, FEATURE_VEC_SIZE)) {
                        // 特征提取完成后立即喂狗
                        SAFE_WDT_RESET();
                        // 计算当前视图的置信度（使用特征向量的范数作为简单置信度指标）
                        float norm = 0.0f;
                        for (int i = 0; i < FEATURE_VEC_SIZE; i++) {
                            norm += feature_vec[i] * feature_vec[i];
                        }
                        norm = sqrtf(norm);
                        
                        // 计算完成后喂狗
                        SAFE_WDT_RESET();
                        
                        if (msg.cmd == CMD_CAPTURE_FRONT) {
                            memcpy(g_front_feature, feature_vec, sizeof(g_front_feature));
                            g_inventory_confidence[0] = norm;
                            ESP_LOGI(TAG, "Front view captured, confidence: %.4f", norm);
                            
                            // 如果在盘点模式下，更新状态并引导下一步
                            if (g_inventory_state == INVENTORY_WAITING_FRONT) {
                                g_inventory_state = INVENTORY_WAITING_SIDE;
                                uart_write_bytes(UART_NUM, (const char *)"\r\n[STEP 2/3] Please capture SIDE view\r\n", 38);
                                uart_write_bytes(UART_NUM, (const char *)"         Send 's' to capture\r\n", 29);
                            }
                        }
                        if (msg.cmd == CMD_CAPTURE_SIDE) {
                            memcpy(g_side_feature, feature_vec, sizeof(g_side_feature));
                            g_inventory_confidence[1] = norm;
                            ESP_LOGI(TAG, "Side view captured, confidence: %.4f", norm);
                            
                            // 如果在盘点模式下，更新状态并引导下一步
                            if (g_inventory_state == INVENTORY_WAITING_SIDE) {
                                g_inventory_state = INVENTORY_WAITING_TOP;
                                uart_write_bytes(UART_NUM, (const char *)"\r\n[STEP 3/3] Please capture TOP view\r\n", 38);
                                uart_write_bytes(UART_NUM, (const char *)"         Send 't' to capture and analyze\r\n", 42);
                            }
                        }
                        if (msg.cmd == CMD_CAPTURE_TOP) {
                            memcpy(g_top_feature, feature_vec, sizeof(g_top_feature));
                            g_inventory_confidence[2] = norm;
                            ESP_LOGI(TAG, "Top view captured, confidence: %.4f", norm);
                            
                            // 如果在盘点模式下，触发分析
                            if (g_inventory_state == INVENTORY_WAITING_TOP) {
                                g_inventory_state = INVENTORY_ANALYZING;
                                
                                // 执行加权综合判断
                                uart_write_bytes(UART_NUM, (const char *)"\r\n[ANALYZING] Computing weighted confidence...\r\n", 46);
                                
                                // 计算加权综合置信度
                                const float weights[3] = {0.5f, 0.3f, 0.2f}; // 正面、侧面、顶部
                                float weighted_confidence = 0.0f;
                                float total_weight = 0.0f;
                                
                                for (int i = 0; i < 3; i++) {
                                    if (g_inventory_confidence[i] > 0) {
                                        weighted_confidence += g_inventory_confidence[i] * weights[i];
                                        total_weight += weights[i];
                                    }
                                }
                                
                                if (total_weight > 0) {
                                    weighted_confidence /= total_weight;
                                }
                                
                                ESP_LOGI(TAG, "Inventory Analysis Result:");
                                ESP_LOGI(TAG, "  Front confidence: %.4f (weight: %.1f)", g_inventory_confidence[0], weights[0]);
                                ESP_LOGI(TAG, "  Side confidence:  %.4f (weight: %.1f)", g_inventory_confidence[1], weights[1]);
                                ESP_LOGI(TAG, "  Top confidence:   %.4f (weight: %.1f)", g_inventory_confidence[2], weights[2]);
                                ESP_LOGI(TAG, "  Weighted综合置信度: %.4f", weighted_confidence);
                                
                                char result_msg[256];
                                snprintf(result_msg, sizeof(result_msg), 
                                         "\r\n========== INVENTORY RESULT ==========\r\n"
                                         "  Front: %.2f (×0.5)\r\n"
                                         "  Side:  %.2f (×0.3)\r\n"
                                         "  Top:   %.2f (×0.2)\r\n"
                                         "  ----------------------------------------\r\n"
                                         "  Weighted Confidence: %.4f\r\n"
                                         "  MAC: %s\r\n"
                                         "========================================\r\n",
                                         g_inventory_confidence[0], 
                                         g_inventory_confidence[1],
                                         g_inventory_confidence[2],
                                         weighted_confidence,
                                         msg.mac);
                                uart_write_bytes(UART_NUM, (const char *)result_msg, strlen(result_msg));
                                
                                // 保存资产到SD卡
                                system_msg_t save_msg = {0};
                                save_msg.cmd = CMD_SAVE_ASSET;
                                strncpy(save_msg.mac, msg.mac, MAC_ADDR_LEN);
                                
                                asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
                                if (record) {
                                    strncpy(record->mac_address, msg.mac, MAC_ADDR_LEN);
                                    memcpy(record->front_feature, g_front_feature, sizeof(g_front_feature));
                                    memcpy(record->side_feature, g_side_feature, sizeof(g_side_feature));
                                    memcpy(record->top_feature, g_top_feature, sizeof(g_top_feature));
                                    record->is_valid = true;
                                    
                                    save_msg.data = record;
                                    xQueueSend(xStorageQueue, &save_msg, portMAX_DELAY);
                                }
                                
                                // 重置盘点状态
                                g_inventory_state = INVENTORY_IDLE;
                                memset(g_inventory_confidence, 0, sizeof(g_inventory_confidence));
                            }
                        }
                        
                        uart_write_bytes(UART_NUM, (const char *)"Feature extracted successfully\r\n", 34);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"Failed to extract feature\r\n", 28);
                        if (feature_vec) free(feature_vec);
                    }
                    break;

                case CMD_START_INVENTORY:
                    // 开始盘点模式前喂狗
                    SAFE_WDT_RESET();
                    
                    if (!g_camera_ready) {
                        uart_write_bytes(UART_NUM, (const char *)"Camera not ready!\r\n", 20);
                        break;
                    }
                    
                    // 初始化盘点状态
                    g_inventory_state = INVENTORY_WAITING_FRONT;
                    memset(g_inventory_confidence, 0, sizeof(g_inventory_confidence));
                    
                    ESP_LOGI(TAG, "=== Starting Inventory Mode (Manual Step-by-Step) ===");
                    uart_write_bytes(UART_NUM, (const char *)"\r\n========== INVENTORY MODE ==========\r\n", 37);
                    uart_write_bytes(UART_NUM, (const char *)"[STEP 1/3] Please capture FRONT view\r\n", 39);
                    uart_write_bytes(UART_NUM, (const char *)"         Send 'f' to capture\r\n", 30);
                    uart_write_bytes(UART_NUM, (const char *)"====================================\r\n", 37);
                    
                    break;
                
                default:
                    ESP_LOGW(TAG, "AI task received unknown command: %d", msg.cmd);
                    break;
            }
        } else {
            // 即使没有收到消息，也要定期喂狗
            SAFE_WDT_RESET();
        }
    }
}

// ========== 存储任务：独立处理SD卡操作，避免堆竞争 ==========
static void storage_task(void *pvParameters)
{
    // 注册当前任务到看门狗监控列表
    esp_task_wdt_add(NULL);
    
    system_msg_t msg;
    
    while (1) {
        if (xQueueReceive(xStorageQueue, &msg, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            
            switch (msg.cmd) {
                case CMD_INIT_STORAGE:
                    if (storage_module_init()) {
                        g_storage_ready = true;
                        g_storage_initialized = true;
                        ESP_LOGI(TAG, "Storage task: SD card ready.");
                    } else {
                        g_storage_ready = false;
                        ESP_LOGE(TAG, "Storage task: SD init failed");
                    }
                    break;

                case CMD_SAVE_ASSET:
                    if (g_storage_ready && msg.data) {
                        ESP_LOGI(TAG, "Storage task: Saving asset for MAC: %s", msg.mac);
                        if (storage_module_save_asset((asset_record_t *)msg.data)) {
                            uart_write_bytes(UART_NUM, (const char *)"✅ Asset saved to SD card\r\n", 29);
                        } else {
                            uart_write_bytes(UART_NUM, (const char *)"❌ Failed to save asset\r\n", 27);
                        }
                        free(msg.data); 
                    }
                    break;
                
                default:
                    ESP_LOGW(TAG, "Storage task received unknown command: %d", msg.cmd);
                    break;
            }
        } else {
            // 即使没有收到消息，也要定期喂狗
            SAFE_WDT_RESET();
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Initializing ESP32-CAM AI System ===");
    
    init_uart();
    
    // 初始化 AI 模块（模型加载）
    ai_module_init();

    // 创建队列
    xSystemQueue = xQueueCreate(UART_QUEUE_LEN, sizeof(system_msg_t));
    xStorageQueue = xQueueCreate(STORAGE_QUEUE_LEN, sizeof(system_msg_t));

    // 创建任务 (优先级: Storage > AI > UART)
    xTaskCreatePinnedToCore(storage_task, "storage_task", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(camera_ai_task, "camera_ai_task", 16384, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 3, NULL, 1);

    uart_write_bytes(UART_NUM, (const char *)"\r\n[SYSTEM] ESP32-CAM AI System Ready\r\n", 38);
    uart_write_bytes(UART_NUM, (const char *)"[GUIDE] Please input MAC address (Format: XX:XX:XX:XX:XX:XX)\r\n", 62);
    uart_write_bytes(UART_NUM, (const char *)"[GUIDE] After initialization:\r\n", 31);
    uart_write_bytes(UART_NUM, (const char *)"         'f'=Front, 's'=Side, 't'=Top (Manual)\r\n", 47);
    uart_write_bytes(UART_NUM, (const char *)"         'c'=Inventory Mode (Step-by-step guide)\r\n", 49);
    uart_write_bytes(UART_NUM, (const char *)"         'l'=List all assets\r\n", 27);
    uart_write_bytes(UART_NUM, (const char *)"         'i'=System info\r\n", 23);
    uart_write_bytes(UART_NUM, (const char *)"         'help'/'?': Show help\r\n", 28);
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
