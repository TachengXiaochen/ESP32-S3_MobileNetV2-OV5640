#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
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

#include "mobilenet_wrapper.h"
#include "asset_manager.h"

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

// 特征向量相似度阈值 (调整为更合适的数值)
#define COSINE_THRESHOLD 0.85f

// MAC地址最大长度已在asset_manager.h中定义，此处不再重复定义
// 使用 asset_manager.h 中的 MAC_ADDR_LEN (17字符: AA:BB:CC:DD:EE:FF)

// 摄像头状态
typedef enum {
    CAM_STATE_IDLE,           // 空闲状态，等待MAC地址
    CAM_STATE_WAITING_MAC,    // 等待MAC地址输入
    CAM_STATE_READY,          // MAC地址已输入，摄像头可用
} camera_state_t;

// 三视图拍摄状态
typedef enum {
    VIEW_NONE = 0,
    VIEW_FRONT = 1,     // 正面
    VIEW_SIDE = 2,      // 侧面
    VIEW_TOP = 3,       // 顶部
    VIEW_COMPLETE = 4   // 三视图完成
} view_state_t;

// ESP32-S3 常见 OV5640 摄像头引脚定义
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_V_SYNC 6
#define CAM_PIN_H_SYNC 7
#define CAM_PIN_PCLK   13

static const char *TAG = "camera_ai";

// 全局特征向量存储（1280 维）
#define FEATURE_VEC_SIZE 1280
static float g_stored_feature[FEATURE_VEC_SIZE] = {0};
static bool g_feature_stored = false;

// MAC地址存储
static char g_current_mac[MAC_ADDR_LEN + 1] = {0};
static camera_state_t g_camera_state = CAM_STATE_WAITING_MAC;

// 存储初始化状态标志
static bool g_storage_initialized = false;

// 摄像头电源状态标志
static bool g_camera_power_on = false;

// 三视图特征存储
static float g_front_feature[FEATURE_VEC_SIZE] = {0};
static float g_side_feature[FEATURE_VEC_SIZE] = {0};
static float g_top_feature[FEATURE_VEC_SIZE] = {0};
static view_state_t g_view_state = VIEW_NONE;

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

static void init_camera(void)
{
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer = LEDC_TIMER_0,
        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_pclk = CAM_PIN_PCLK,
        .pin_vsync = CAM_PIN_V_SYNC,
        .pin_href = CAM_PIN_H_SYNC,
        .pin_sscb_sda = CAM_PIN_SIOD,
        .pin_sscb_scl = CAM_PIN_SIOC,
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .xclk_freq_hz = 10000000,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_96X96,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return;
    }
    ESP_LOGI(TAG, "Camera init OK");
}

// 余弦相似度计算
static float cosine_similarity(const float *a, const float *b, size_t len)
{
    float dot_product = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;
    
    for (size_t i = 0; i < len; i++) {
        dot_product += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    norm_a = sqrtf(norm_a);
    norm_b = sqrtf(norm_b);
    
    if (norm_a < 1e-6f || norm_b < 1e-6f) {
        return 0.0f;
    }
    
    return dot_product / (norm_a * norm_b);
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

// 从MobileNetV2模型提取特征向量
static bool extract_feature_from_mobilenet(float *feature_vec, size_t feature_size)
{
    ESP_LOGI(TAG, "Extracting features using MobileNetV2 wrapper...");
    return mobilenet_extract_features(feature_vec, feature_size);
}

// 关闭摄像头(节能模式)
static void camera_power_off(void)
{
    if (g_camera_power_on) {
        ESP_LOGI(TAG, "Powering off camera for energy saving...");
        esp_err_t ret = esp_camera_deinit();
        if (ret == ESP_OK) {
            g_camera_power_on = false;
            uart_write_bytes(UART_NUM, (const char *)"Camera powered OFF (energy saving mode)\r\n", 41);
            ESP_LOGI(TAG, "Camera deinitialized successfully");
        } else {
            uart_write_bytes(UART_NUM, (const char *)"WARNING: Failed to power off camera\r\n", 37);
            ESP_LOGE(TAG, "esp_camera_deinit failed: 0x%x", ret);
        }
    } else {
        ESP_LOGI(TAG, "Camera already powered off");
    }
}

// 唤醒摄像头
static esp_err_t camera_power_on(void)
{
    if (!g_camera_power_on) {
        ESP_LOGI(TAG, "Powering on camera...");
        uart_write_bytes(UART_NUM, (const char *)"Waking up camera...\r\n", 21);
        
        // 重新初始化摄像头
        init_camera();
        
        // 延迟等待DMA稳定
        vTaskDelay(pdMS_TO_TICKS(300));
        
        g_camera_power_on = true;
        uart_write_bytes(UART_NUM, (const char *)"Camera powered ON\r\n", 20);
        ESP_LOGI(TAG, "Camera reinitialized successfully");
        return ESP_OK;
    }
    return ESP_OK; // 已经开启
}

// 处理串口命令 - 支持MAC地址输入和WiFi控制
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
    }
    
    // MAC地址输入（仅在等待MAC状态时接受）
    if (g_camera_state == CAM_STATE_WAITING_MAC) {
        if (validate_mac_address(cmd)) {
            strncpy(g_current_mac, cmd, MAC_ADDR_LEN);
            
            // 复位看门狗（防止长耗时操作导致重启）
            SAFE_WDT_RESET();
            
            // ========== 阶段2: 硬件初始化(串行化,避免并发) ==========
            
            // 1. 初始化摄像头
            uart_write_bytes(UART_NUM, (const char *)"Initializing camera...\r\n", 26);
            init_camera();
            g_camera_power_on = true;  // 标记摄像头已开启
            
            // 复位看门狗
            SAFE_WDT_RESET();
            
            // 2. 延迟500ms让摄像头DMA稳定
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // 3. 初始化SD卡存储
            uart_write_bytes(UART_NUM, (const char *)"Initializing SD card...\r\n", 26);
            esp_err_t ret = asset_manager_init();
            if (ret != ESP_OK) {
                uart_write_bytes(UART_NUM, (const char *)"Storage init FAILED. Continuing without storage.\r\n", 50);
                ESP_LOGW(TAG, "SD card initialization failed (0x%x). System will run without persistence.", ret);
                
                // 关键修复：禁用SD卡模式，防止后续操作尝试访问无效的存储
                g_storage_initialized = false;
            } else {
                uart_write_bytes(UART_NUM, (const char *)"Storage initialized\r\n", 22);
                g_storage_initialized = true;
                
                // 检查该MAC是否已存在
                asset_record_t existing_record;
                ret = asset_load(g_current_mac, &existing_record);
                if (ret == ESP_OK) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "WARNING: MAC %s already registered!\r\n", g_current_mac);
                    uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
                    uart_write_bytes(UART_NUM, (const char *)"Overwriting existing record...\r\n", 35);
                }
            }
            
            // 复位看门狗
            SAFE_WDT_RESET();
            
            g_camera_state = CAM_STATE_READY;
            
            char msg[128];
            snprintf(msg, sizeof(msg), "MAC address set: %s\r\n", g_current_mac);
            uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
            
            // 提示用户可以进行三视图拍摄
            uart_write_bytes(UART_NUM, (const char *)"=== Asset Registration Mode ===\r\n", 34);
            uart_write_bytes(UART_NUM, (const char *)"Send 'f' to capture FRONT view\r\n", 34);
            uart_write_bytes(UART_NUM, (const char *)"Send 's' to capture SIDE view\r\n", 32);
            uart_write_bytes(UART_NUM, (const char *)"Send 't' to capture TOP view\r\n", 31);
            uart_write_bytes(UART_NUM, (const char *)"Send 'c' to check inventory\r\n", 30);
            uart_write_bytes(UART_NUM, (const char *)"Send 'l' to list all assets\r\n", 30);
            uart_write_bytes(UART_NUM, (const char *)"Send 'p' to power OFF camera (save energy)\r\n", 45);
            uart_write_bytes(UART_NUM, (const char *)"Send 'w' to wake UP camera\r\n", 29);
            uart_write_bytes(UART_NUM, (const char *)"Send 'r' to reset and input new MAC\r\n", 37);

        } else {
            uart_write_bytes(UART_NUM, (const char *)"Invalid MAC format. Use XX:XX:XX:XX:XX:XX\r\n", 42);
        }
        return;
    }

    // 摄像头就绪状态下的命令处理
    if (g_camera_state == CAM_STATE_READY) {
        switch (cmd[0]) {
            case 'f':
            case 'F':
                // 拍摄正面视图
                if (g_view_state >= VIEW_FRONT) {
                    uart_write_bytes(UART_NUM, (const char *)"Front view already captured\r\n", 31);
                    break;
                }
                ESP_LOGI(TAG, "Capturing FRONT view for MAC: %s", g_current_mac);
                uart_write_bytes(UART_NUM, (const char *)"Preparing to capture FRONT view...\r\n", 38);
                
                if (extract_feature_from_mobilenet(g_front_feature, FEATURE_VEC_SIZE)) {
                    g_view_state = VIEW_FRONT;
                    uart_write_bytes(UART_NUM, (const char *)"FRONT view captured successfully\r\n", 36);
                    uart_write_bytes(UART_NUM, (const char *)"Next: Send 's' to capture SIDE view\r\n", 37);
                    ESP_LOGI(TAG, "Front view feature stored");
                } else {
                    uart_write_bytes(UART_NUM, (const char *)"FRONT view capture FAILED\r\n", 28);
                    ESP_LOGE(TAG, "Failed to capture front view");
                }
                break;
                
            case 's':
            case 'S':
                // 拍摄侧面视图
                if (g_view_state < VIEW_FRONT) {
                    uart_write_bytes(UART_NUM, (const char *)"Please capture FRONT view first\r\n", 34);
                    break;
                }
                if (g_view_state >= VIEW_SIDE) {
                    uart_write_bytes(UART_NUM, (const char *)"Side view already captured\r\n", 30);
                    break;
                }
                ESP_LOGI(TAG, "Capturing SIDE view for MAC: %s", g_current_mac);
                uart_write_bytes(UART_NUM, (const char *)"Preparing to capture SIDE view...\r\n", 37);
                
                if (extract_feature_from_mobilenet(g_side_feature, FEATURE_VEC_SIZE)) {
                    g_view_state = VIEW_SIDE;
                    uart_write_bytes(UART_NUM, (const char *)"SIDE view captured successfully\r\n", 35);
                    uart_write_bytes(UART_NUM, (const char *)"Next: Send 't' to capture TOP view\r\n", 36);
                    ESP_LOGI(TAG, "Side view feature stored");
                } else {
                    uart_write_bytes(UART_NUM, (const char *)"SIDE view capture FAILED\r\n", 27);
                    ESP_LOGE(TAG, "Failed to capture side view");
                }
                break;
                
            case 't':
            case 'T':
                // 拍摄顶部视图
                if (g_view_state < VIEW_SIDE) {
                    uart_write_bytes(UART_NUM, (const char *)"Please capture FRONT and SIDE views first\r\n", 42);
                    break;
                }
                if (g_view_state >= VIEW_TOP) {
                    uart_write_bytes(UART_NUM, (const char *)"Top view already captured\r\n", 29);
                    break;
                }
                ESP_LOGI(TAG, "Capturing TOP view for MAC: %s", g_current_mac);
                uart_write_bytes(UART_NUM, (const char *)"Preparing to capture TOP view...\r\n", 36);
                
                if (extract_feature_from_mobilenet(g_top_feature, FEATURE_VEC_SIZE)) {
                    g_view_state = VIEW_TOP;
                    uart_write_bytes(UART_NUM, (const char *)"TOP view captured successfully\r\n", 34);
                    
                    // 【关键修复】推理完成后增加额外延迟,确保堆管理器稳定
                    // 这是防止fopen崩溃的关键步骤
                    ESP_LOGI(TAG, "Waiting for heap stabilization after inference...");
                    vTaskDelay(pdMS_TO_TICKS(500));  // 额外等待500ms
                    
                    // 复位看门狗，准备保存数据
                    SAFE_WDT_RESET();
                    
                    // 保存资产记录到SD卡（仅在存储已初始化时）
                    if (g_storage_initialized) {
                        ESP_LOGI(TAG, "Starting asset save operation...");
                        
                        // 【关键修复】使用静态变量避免栈溢出(15KB结构体!)
                        static asset_record_t record;
                        strncpy(record.mac_address, g_current_mac, MAC_ADDR_LEN);
                        record.mac_address[MAC_ADDR_LEN] = '\0';
                        memcpy(record.front_feature, g_front_feature, sizeof(g_front_feature));
                        memcpy(record.side_feature, g_side_feature, sizeof(g_side_feature));
                        memcpy(record.top_feature, g_top_feature, sizeof(g_top_feature));
                        record.is_valid = true;
                        
                        esp_err_t ret = asset_save(&record);
                        if (ret == ESP_OK) {
                            uart_write_bytes(UART_NUM, (const char *)"Asset SAVED to SD card\r\n", 26);
                            ESP_LOGI(TAG, "Asset saved successfully");
                        } else {
                            uart_write_bytes(UART_NUM, (const char *)"WARNING: Failed to save to SD card\r\n", 36);
                            ESP_LOGE(TAG, "asset_save failed with error: 0x%x", ret);
                        }
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"SKIPPED: Storage not initialized\r\n", 35);
                        ESP_LOGW(TAG, "Asset registration completed but NOT saved (storage unavailable)");
                    }
                    
                    uart_write_bytes(UART_NUM, (const char *)"=== All three views completed! ===\r\n", 36);
                    uart_write_bytes(UART_NUM, (const char *)"Asset registered. Send 'c' for inventory check\r\n", 50);
                    ESP_LOGI(TAG, "Top view feature stored - registration complete");
                    
                    // ========== 节能功能: 三视图完成后自动关闭摄像头 ==========
                    vTaskDelay(pdMS_TO_TICKS(200)); // 短暂延迟让用户看到完成提示
                    camera_power_off();
                    uart_write_bytes(UART_NUM, (const char *)"\r\n[Energy Saving] Camera auto-off after registration\r\n", 54);
                    uart_write_bytes(UART_NUM, (const char *)"Send 'w' to wake up camera when needed\r\n", 41);
                } else {
                    uart_write_bytes(UART_NUM, (const char *)"TOP view capture FAILED\r\n", 27);
                    ESP_LOGE(TAG, "Failed to capture top view");
                }
                break;
                
            case 'c':
            case 'C':
                // 盘点检查
                if (g_view_state != VIEW_TOP) {
                    uart_write_bytes(UART_NUM, (const char *)"Registration incomplete. Please capture all 3 views first\r\n", 60);
                    break;
                }
                ESP_LOGI(TAG, "Starting inventory check for MAC: %s", g_current_mac);
                uart_write_bytes(UART_NUM, (const char *)"=== Inventory Check Mode ===\r\n", 31);
                uart_write_bytes(UART_NUM, (const char *)"Please position the item and send:\r\n", 39);
                uart_write_bytes(UART_NUM, (const char *)"  '1' - Capture FRONT for comparison\r\n", 39);
                uart_write_bytes(UART_NUM, (const char *)"  '2' - Capture SIDE for comparison\r\n", 38);
                uart_write_bytes(UART_NUM, (const char *)"  '3' - Capture TOP for comparison\r\n", 37);
                break;
                
            case 'l':
            case 'L':
                // 列出所有资产
                ESP_LOGI(TAG, "Listing all registered assets");
                uart_write_bytes(UART_NUM, (const char *)"\r\n", 2);
                
                int count = 0;
                esp_err_t ret = asset_list_all(&count);
                if (ret != ESP_OK) {
                    uart_write_bytes(UART_NUM, (const char *)"Failed to list assets\r\n", 25);
                }
                break;
                
            case '1':
                // 盘点：比对正面视图
                if (g_view_state != VIEW_TOP) {
                    uart_write_bytes(UART_NUM, (const char *)"Registration not complete\r\n", 28);
                    break;
                }
                
                // 节能功能: 如果摄像头已关闭,先唤醒
                if (!g_camera_power_on) {
                    camera_power_on();
                }
                
                ESP_LOGI(TAG, "Inventory: Capturing FRONT view for comparison");
                uart_write_bytes(UART_NUM, (const char *)"Capturing FRONT view for comparison...\r\n", 41);
                
                {
                    static float current_feature[FEATURE_VEC_SIZE];
                    if (extract_feature_from_mobilenet(current_feature, FEATURE_VEC_SIZE)) {
                        float similarity = cosine_similarity(g_front_feature, current_feature, FEATURE_VEC_SIZE);
                        char msg[128];
                        snprintf(msg, sizeof(msg), "FRONT similarity: %.4f ", similarity);
                        
                        if (similarity >= COSINE_THRESHOLD) {
                            strcat(msg, "[MATCH]\r\n");
                        } else {
                            strcat(msg, "[NO MATCH]\r\n");
                        }
                        uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
                        ESP_LOGI(TAG, "Front view similarity: %.4f", similarity);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"FRONT capture failed\r\n", 23);
                    }
                }
                break;
                
            case '2':
                // 盘点：比对侧面视图
                if (g_view_state != VIEW_TOP) {
                    uart_write_bytes(UART_NUM, (const char *)"Registration not complete\r\n", 28);
                    break;
                }
                
                // 节能功能: 如果摄像头已关闭,先唤醒
                if (!g_camera_power_on) {
                    camera_power_on();
                }
                
                ESP_LOGI(TAG, "Inventory: Capturing SIDE view for comparison");
                uart_write_bytes(UART_NUM, (const char *)"Capturing SIDE view for comparison...\r\n", 40);
                
                {
                    static float current_feature[FEATURE_VEC_SIZE];
                    if (extract_feature_from_mobilenet(current_feature, FEATURE_VEC_SIZE)) {
                        float similarity = cosine_similarity(g_side_feature, current_feature, FEATURE_VEC_SIZE);
                        char msg[128];
                        snprintf(msg, sizeof(msg), "SIDE similarity: %.4f ", similarity);
                        
                        if (similarity >= COSINE_THRESHOLD) {
                            strcat(msg, "[MATCH]\r\n");
                        } else {
                            strcat(msg, "[NO MATCH]\r\n");
                        }
                        uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
                        ESP_LOGI(TAG, "Side view similarity: %.4f", similarity);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"SIDE capture failed\r\n", 22);
                    }
                }
                break;
                
            case '3':
                // 盘点：比对顶部视图
                if (g_view_state != VIEW_TOP) {
                    uart_write_bytes(UART_NUM, (const char *)"Registration not complete\r\n", 28);
                    break;
                }
                
                // 节能功能: 如果摄像头已关闭,先唤醒
                if (!g_camera_power_on) {
                    camera_power_on();
                }
                
                ESP_LOGI(TAG, "Inventory: Capturing TOP view for comparison");
                uart_write_bytes(UART_NUM, (const char *)"Capturing TOP view for comparison...\r\n", 39);
                
                {
                    static float current_feature[FEATURE_VEC_SIZE];
                    if (extract_feature_from_mobilenet(current_feature, FEATURE_VEC_SIZE)) {
                        float similarity = cosine_similarity(g_top_feature, current_feature, FEATURE_VEC_SIZE);
                        char msg[128];
                        snprintf(msg, sizeof(msg), "TOP similarity: %.4f ", similarity);
                        
                        if (similarity >= COSINE_THRESHOLD) {
                            strcat(msg, "[MATCH]\r\n");
                        } else {
                            strcat(msg, "[NO MATCH]\r\n");
                        }
                        uart_write_bytes(UART_NUM, (const char *)msg, strlen(msg));
                        ESP_LOGI(TAG, "Top view similarity: %.4f", similarity);
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"TOP capture failed\r\n", 21);
                    }
                }
                break;
                
            case 'p':
            case 'P':
                // 手动关闭摄像头(节能)
                ESP_LOGI(TAG, "Manual camera power off requested");
                camera_power_off();
                break;
                
            case 'w':
            case 'W':
                // 手动唤醒摄像头
                if (g_camera_state != CAM_STATE_READY) {
                    uart_write_bytes(UART_NUM, (const char *)"System not ready. Please input MAC first\r\n", 42);
                    break;
                }
                ESP_LOGI(TAG, "Manual camera wake up requested");
                camera_power_on();
                break;
                
            case 'r':
            case 'R':
                // 重置状态，重新输入MAC地址
                g_camera_state = CAM_STATE_WAITING_MAC;
                g_view_state = VIEW_NONE;
                memset(g_current_mac, 0, sizeof(g_current_mac));
                memset(g_front_feature, 0, sizeof(g_front_feature));
                memset(g_side_feature, 0, sizeof(g_side_feature));
                memset(g_top_feature, 0, sizeof(g_top_feature));
                
                // 关闭摄像头
                if (g_camera_power_on) {
                    camera_power_off();
                }
                
                ESP_LOGI(TAG, "System reset (Storage remains active)");
                
                uart_write_bytes(UART_NUM, (const char *)"=== System Reset ===\r\n", 23);
                uart_write_bytes(UART_NUM, (const char *)"Please input MAC address (format: XX:XX:XX:XX:XX:XX):\r\n", 58);
                break;
                
            default:
                uart_write_bytes(UART_NUM, (const char *)"Unknown command\r\n", 17);
                break;
        }
    }
}

// UART 接收任务 - 修改为按行读取
static void uart_task(void *pvParameters)
{
    // 注册当前任务到看门狗监控列表
    SAFE_WDT_ADD();
    
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

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Initializing ESP32-CAM AI System with MobileNetV2 ===");
    
    init_uart();
    ESP_LOGI(TAG, "UART initialized");
    
    // 注意：此时不初始化摄像头，等待MAC地址输入后再启动
    
    // 初始化MobileNetV2模型
    ESP_LOGI(TAG, "Initializing MobileNetV2 model...");
    if (mobilenet_init()) {
        ESP_LOGI(TAG, "MobileNetV2 model initialized successfully");
    } else {
        ESP_LOGW(TAG, "Failed to initialize MobileNetV2 model");
    }
    
    // 创建 UART 接收任务（增加栈大小防止溢出）
    xTaskCreatePinnedToCore(uart_task, "uart_task", 8192, NULL, 10, NULL, 1);

    uart_write_bytes(UART_NUM, (const char *)"\r\n=== ESP32-CAM AI Asset Management System ===\r\n", 50);
    uart_write_bytes(UART_NUM, (const char *)"Please input MAC address (format: XX:XX:XX:XX:XX:XX):\r\n", 58);
    uart_write_bytes(UART_NUM, (const char *)"\r\n", 2);
    uart_write_bytes(UART_NUM, (const char *)"Quick Reference:\r\n", 18);
    uart_write_bytes(UART_NUM, (const char *)"  Registration: f(front) -> s(side) -> t(top)\r\n", 49);
    uart_write_bytes(UART_NUM, (const char *)"  Inventory:  c(enter mode) -> 1/2/3(compare)\r\n", 49);
    uart_write_bytes(UART_NUM, (const char *)"  Power:      p(off) / w(on) - Auto off after registration\r\n", 63);
    uart_write_bytes(UART_NUM, (const char *)"  Storage:    storage sd/flash/status\r\n", 38);
    uart_write_bytes(UART_NUM, (const char *)"  Reset:      r - Clear all and restart\r\n", 39);
    uart_write_bytes(UART_NUM, (const char *)"\r\n", 2);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
