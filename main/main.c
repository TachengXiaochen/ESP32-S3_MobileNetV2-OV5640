#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>    // stat, mkdir
#include <dirent.h>      // opendir, readdir
#include <errno.h>       // errno

#include "esp_system.h"
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
#include "asset_manager.h"
#include "cmd_handler.h"  // 新增：命令处理器模块
#include "led_indicator.h"  // 新增：LED状态指示器
#include "main.h"         // 新增：主模块接口定义

// ========== FreeRTOS 任务与队列定义 ==========
#define UART_QUEUE_LEN    10
#define STORAGE_QUEUE_LEN 5

// UART 配置
#define UART_NUM UART_NUM_0
#define UART_BAUD_RATE 115200
#define UART_BUF_SIZE (1024 * 2)

// 特征向量维度
#define FEATURE_VEC_SIZE 1280

static const char *TAG = "camera_ai";

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

// ========== 全局变量定义（在 main.h 中声明为 extern）==========

// 全局队列句柄
QueueHandle_t xSystemQueue = NULL;
QueueHandle_t xStorageQueue = NULL;

// Global state variables
bool g_camera_ready = false;
bool g_storage_ready = false;

// Shared state for multi-view capture
float g_front_feature[FEATURE_VEC_SIZE] = {0};
float g_side_feature[FEATURE_VEC_SIZE] = {0};
float g_top_feature[FEATURE_VEC_SIZE] = {0};

// 盘点模式状态
inventory_state_t g_inventory_state = INVENTORY_IDLE;

char g_current_mac[MAC_ADDR_LEN + 1] = {0};
camera_state_t g_camera_state = CAM_STATE_WAITING_MAC;
view_state_t g_view_state = VIEW_NONE;
bool g_camera_power_on = false;
bool g_storage_initialized = false;
bool g_is_inventory_mode = false;  // ✅ 新增：区分注册和盘点模式的标志位

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

// 打印系统信息到UART
void print_system_info_uart(void)
{
    char info_buf[512];
    
    // 获取内存信息
    const uint32_t free_heap = esp_get_free_heap_size();
    const uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    
    snprintf(info_buf, sizeof(info_buf),
             "\r\n========== SYSTEM INFORMATION ==========\r\n"
             "  Chip Model:     ESP32-S3\r\n"
             "  CPU Cores:      2\r\n"
             "  Free Heap:      %lu bytes\r\n"
             "  Min Free Heap:  %lu bytes\r\n"
             "  Camera State:   %s\r\n"
             "  Storage State:  %s\r\n"
             "  Current MAC:    %s\r\n"
             "  Mode:           %s\r\n"
             "===========================================\r\n",
             (unsigned long)free_heap,
             (unsigned long)min_free_heap,
             g_camera_ready ? "READY" : "NOT READY",
             g_storage_ready ? "READY" : "NOT READY",
             strlen(g_current_mac) > 0 ? g_current_mac : "N/A",
             g_is_inventory_mode ? "INVENTORY" : "REGISTRATION");
    
    uart_write_bytes(UART_NUM, (const char *)info_buf, strlen(info_buf));
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
                        
                        // 使用 cmd_handler 统一处理所有命令
                        extern void cmd_handler_process(const char *cmd_line);
                        cmd_handler_process(line_buf);
                        
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
        
        // 定期复位看门狗（防止无输入时超时）
        esp_task_wdt_reset();
    }
}

// 摄像头AI处理任务 - 简化版（仅处理队列消息）
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

                case CMD_INIT_STORAGE:
                    // 存储初始化由storage_task处理，此处忽略
                    break;

                case CMD_CAPTURE_FRONT:
                case CMD_CAPTURE_SIDE:
                case CMD_CAPTURE_TOP:
                    if (!g_camera_ready) {
                        uart_write_bytes(UART_NUM, (const char *)"Camera not ready!\r\n", 20);
                        break;
                    }
                    
                    // LED闪烁反馈
                    extern void led_capture_front(bool is_inventory);
                    extern void led_capture_side(bool is_inventory);
                    extern void led_capture_top(bool is_inventory);
                    
                    float *feature_ptr = NULL;
                    const char *view_name = NULL;
                    const char *view_label = NULL;  // 用于图片文件名
                    
                    if (msg.cmd == CMD_CAPTURE_FRONT) {
                        feature_ptr = g_front_feature;
                        view_name = "Front";
                        view_label = "front";
                        led_capture_front(g_is_inventory_mode);  // ✅ 使用标志位判断模式
                    } else if (msg.cmd == CMD_CAPTURE_SIDE) {
                        feature_ptr = g_side_feature;
                        view_name = "Side";
                        view_label = "side";
                        led_capture_side(g_is_inventory_mode);   // ✅ 使用标志位判断模式
                    } else {
                        feature_ptr = g_top_feature;
                        view_name = "Top";
                        view_label = "top";
                        led_capture_top(g_is_inventory_mode);    // ✅ 使用标志位判断模式
                    }
                    
                    // ✅ AI推理前复位看门狗（推理耗时约2-3秒）
                    esp_task_wdt_reset();
                    
                    // ✅ 先捕获JPEG图片（仅注册模式需要保存）
                    uint8_t *jpeg_buf = NULL;
                    size_t jpeg_len = 0;
                    bool image_saved = false;
                    
                    if (!g_is_inventory_mode && g_storage_ready) {
                        // 注册模式且存储就绪，捕获并保存图片
                        if (camera_module_capture_jpeg(&jpeg_buf, &jpeg_len)) {
                            ESP_LOGI(TAG, "Captured JPEG image: %u bytes", (unsigned int)jpeg_len);
                            
                            // 保存图片到SD卡
                            esp_err_t ret = storage_module_save_image(g_current_mac, view_label, jpeg_buf, jpeg_len);
                            if (ret == ESP_OK) {
                                image_saved = true;
                                ESP_LOGI(TAG, "Image saved successfully");
                            } else {
                                ESP_LOGW(TAG, "Failed to save image (err=0x%x), continuing with feature extraction", ret);
                            }
                            
                            // ✅ 释放JPEG缓冲区内存
                            free(jpeg_buf);
                            jpeg_buf = NULL;
                        } else {
                            ESP_LOGW(TAG, "Failed to capture JPEG image, continuing with feature extraction");
                        }
                    }
                    
                    // 提取特征向量（✅ 新增：多帧融合）
                    extern bool feature_processor_add_frame(const float *feature, int feature_size);
                    extern bool feature_processor_get_fused_feature(float *output, int feature_size);
                    extern void feature_processor_clear_buffer(void);
                    
                    const int NUM_FRAMES = 3;  // 采集3帧进行融合
                    
                    // 清空融合缓冲区
                    feature_processor_clear_buffer();
                    
                    // 采集多帧并添加到缓冲区
                    for (int i = 0; i < NUM_FRAMES; i++) {
                        float single_frame[FEATURE_VEC_SIZE];
                        if (camera_module_capture_and_process(single_frame, FEATURE_VEC_SIZE)) {
                            feature_processor_add_frame(single_frame, FEATURE_VEC_SIZE);
                            ESP_LOGI(TAG, "Frame %d/%d captured", i + 1, NUM_FRAMES);
                        } else {
                            ESP_LOGW(TAG, "Failed to capture frame %d/%d", i + 1, NUM_FRAMES);
                        }
                    }
                    
                    // 获取融合后的特征
                    if (feature_processor_get_fused_feature(feature_ptr, FEATURE_VEC_SIZE)) {
                        ESP_LOGI(TAG, "Multi-frame fusion completed (%d frames)", NUM_FRAMES);
                    } else {
                        ESP_LOGW(TAG, "Multi-frame fusion failed");
                        // 融合失败，但可能已经采集了部分帧，继续处理
                    }
                    
                    // 检查是否至少采集了一帧有效数据
                    extern int feature_processor_get_frame_count(void);
                    int frame_count = feature_processor_get_frame_count();
                    
                    if (frame_count > 0) {
                        // 输出拍摄成功日志
                        char log_msg[128];
                        if (image_saved) {
                            snprintf(log_msg, sizeof(log_msg), "%s view captured (with image), confidence: 1.0000\r\n", view_name);
                        } else {
                            snprintf(log_msg, sizeof(log_msg), "%s view captured, confidence: 1.0000\r\n", view_name);
                        }
                        uart_write_bytes(UART_NUM, (const char *)log_msg, strlen(log_msg));
                        
                        // ✅ AI推理后再次复位看门狗
                        esp_task_wdt_reset();
                        
                        // 更新视图状态并显示下一步引导
                        if (msg.cmd == CMD_CAPTURE_FRONT) {
                            g_view_state = VIEW_FRONT;
                            if (g_inventory_state == INVENTORY_IDLE) {
                                g_inventory_state = INVENTORY_WAITING_SIDE;
                                // 显示第二步引导
                                extern void show_registration_step2(void);
                                show_registration_step2();
                            } else if (g_inventory_state == INVENTORY_WAITING_FRONT) {
                                g_inventory_state = INVENTORY_WAITING_SIDE;
                                // 显示盘点第二步引导
                                extern void show_inventory_step2(void);
                                show_inventory_step2();
                            }
                        } else if (msg.cmd == CMD_CAPTURE_SIDE) {
                            g_view_state = VIEW_SIDE;
                            if (g_inventory_state == INVENTORY_WAITING_SIDE) {
                                g_inventory_state = INVENTORY_WAITING_TOP;
                                // 显示第三步引导
                                extern void show_registration_step3(void);
                                show_registration_step3();
                            } else if (g_inventory_state == INVENTORY_WAITING_SIDE) {
                                // 盘点模式
                                extern void show_inventory_step3(void);
                                show_inventory_step3();
                            }
                        } else {
                            g_view_state = VIEW_TOP;
                            
                            // ✅ 通过检查进入拍摄流程时的初始状态来区分注册和盘点模式
                            // 注册模式: g_inventory_state 初始为 INVENTORY_IDLE
                            // 盘点模式: g_inventory_state 初始为 INVENTORY_WAITING_FRONT
                            
                            if (g_inventory_state == INVENTORY_COMPLETE || 
                                g_inventory_state == INVENTORY_ANALYZING) {
                                // 已经在处理中，忽略重复命令
                                break;
                            }
                            
                            // 🔍 判断当前是注册还是盘点模式
                            // 由于状态机已经推进到 INVENTORY_WAITING_TOP，我们需要回溯判断
                            // 简单方法：检查是否有预加载的特征数据（盘点模式会预先加载）
                            
                            // 💡 更好的方法：添加一个全局标志位
                            extern bool g_is_inventory_mode;  // 新增标志
                            
                            // ✅ 调试日志：输出当前模式
                            ESP_LOGI(TAG, "Current mode: %s (g_is_inventory_mode=%d)", 
                                     g_is_inventory_mode ? "INVENTORY" : "REGISTRATION", g_is_inventory_mode);
                            
                            if (g_is_inventory_mode) {
                                // ✅ 盘点模式：触发分析
                                g_inventory_state = INVENTORY_ANALYZING;
                                system_msg_t analyze_msg = {0};
                                analyze_msg.cmd = CMD_START_INVENTORY;
                                snprintf(analyze_msg.mac, sizeof(analyze_msg.mac), "%s", g_current_mac);
                                
                                // ✅ 关键修复：发送分析消息到队列
                                ESP_LOGI(TAG, "Sending CMD_START_INVENTORY to queue...");
                                xQueueSend(xSystemQueue, &analyze_msg, portMAX_DELAY);
                                ESP_LOGI(TAG, "CMD_START_INVENTORY sent successfully");
                            } else {
                                // ✅ 注册模式：触发保存操作
                                g_inventory_state = INVENTORY_COMPLETE;
                                
                                system_msg_t save_msg = {0};
                                save_msg.cmd = CMD_SAVE_ASSET;
                                
                                asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
                                if (record) {
                                    // ✅ 修复：使用snprintf确保MAC地址字符串正确终止
                                    snprintf(record->mac_address, sizeof(record->mac_address), "%s", g_current_mac);
                                    memcpy(record->front_feature, g_front_feature, sizeof(g_front_feature));
                                    memcpy(record->side_feature, g_side_feature, sizeof(g_side_feature));
                                    memcpy(record->top_feature, g_top_feature, sizeof(g_top_feature));
                                    record->is_valid = true;
                                    save_msg.data = record;
                                    
                                    xQueueSend(xSystemQueue, &save_msg, portMAX_DELAY);
                                }
                            }
                        }
                    } else {
                        uart_write_bytes(UART_NUM, (const char *)"Feature extraction failed!\r\n", 30);
                    }
                    break;

                case CMD_SAVE_ASSET:
                    if (g_storage_ready && msg.data) {
                        ESP_LOGI(TAG, "Storage task: Saving asset for MAC: %s", msg.mac);
                        save_result_t result = storage_module_save_asset((asset_record_t *)msg.data);
                        
                        if (result != SAVE_RESULT_FAILED) {
                            // 注册完成，显示成功提示并关闭摄像头
                            uart_write_bytes(UART_NUM, (const char *)"\r\n✅ REGISTRATION COMPLETE!\r\n", 30);
                            
                            // ✅ 根据保存结果显示不同的提示
                            if (result == SAVE_RESULT_SUCCESS_OVERWRITE) {
                                uart_write_bytes(UART_NUM, (const char *)"  Asset UPDATED (overwritten) on SD card.\r\n", 44);
                            } else {
                                uart_write_bytes(UART_NUM, (const char *)"  Asset saved to SD card successfully.\r\n", 41);
                            }
                            
                            char mac_msg[64];
                            snprintf(mac_msg, sizeof(mac_msg), "  MAC: %s\r\n", msg.mac);
                            uart_write_bytes(UART_NUM, (const char *)mac_msg, strlen(mac_msg));
                            
                            uart_write_bytes(UART_NUM, (const char *)"  Camera: POWER OFF\r\n\r\n", 25);
                            
                            // LED指示：摄像头关闭 - 红色常亮
                            extern void led_camera_off(void);
                            led_camera_off();
                            
                            // 关闭摄像头电源
                            g_camera_power_on = false;
                            camera_module_deinit();
                            
                            // 重置状态，返回主菜单
                            g_camera_state = CAM_STATE_WAITING_MAC;
                            g_view_state = VIEW_NONE;
                            g_inventory_state = INVENTORY_IDLE;
                            g_is_inventory_mode = false;  // ✅ 重置模式标志
                            
                            // 显示主菜单
                            extern void show_main_menu(void);
                            show_main_menu();
                            
                            free(msg.data);
                        } else {
                            // 保存失败
                            uart_write_bytes(UART_NUM, (const char *)"\r\n❌ FAILED TO SAVE ASSET\r\n", 28);
                            uart_write_bytes(UART_NUM, (const char *)"  Please check SD card and try again.\r\n\r\n", 42);
                            
                            // 保持摄像头开启，允许重试
                            // 不重置状态，用户可以重新拍摄
                        }
                    }
                    break;

                case CMD_INVENTORY_WITH_MAC:
                    // ✅ 此命令已废弃，盘点流程改为在拍摄完t后直接分析
                    if (msg.data) {
                        free(msg.data);
                    }
                    break;

                case CMD_START_INVENTORY:
                    // ✅ 盘点模式：执行特征对比分析
                    {
                        ESP_LOGI(TAG, "Starting inventory analysis for MAC: %s", msg.mac);
                        
                        // 从SD卡加载参考资产记录
                        asset_record_t *ref_record = (asset_record_t *)malloc(sizeof(asset_record_t));
                        if (!ref_record) {
                            uart_write_bytes(UART_NUM, (const char *)"Memory allocation failed!\r\n", 28);
                            break;
                        }
                        
                        esp_err_t ret = asset_load(msg.mac, ref_record);
                        if (ret != ESP_OK) {
                            uart_write_bytes(UART_NUM, (const char *)"Failed to load reference asset!\r\n", 34);
                            free(ref_record);
                            break;
                        }

                        // ✅ 使用新的混合相似度计算方法
                        similarity_result_t front_result = {0};
                        similarity_result_t side_result = {0};
                        similarity_result_t top_result = {0};
                        
                        // 对每个视图进行特征匹配（使用默认资产类别）
                        ai_module_match_features(g_front_feature, ref_record->front_feature, FEATURE_VEC_SIZE,
                                                ASSET_CLASS_UNKNOWN, &front_result);
                        ai_module_match_features(g_side_feature, ref_record->side_feature, FEATURE_VEC_SIZE,
                                                ASSET_CLASS_UNKNOWN, &side_result);
                        ai_module_match_features(g_top_feature, ref_record->top_feature, FEATURE_VEC_SIZE,
                                                ASSET_CLASS_UNKNOWN, &top_result);
                        
                        // 计算加权置信度
                        float weighted_conf = front_result.confidence * 0.5f + 
                                            side_result.confidence * 0.3f + 
                                            top_result.confidence * 0.2f;
                        
                        // 使用前视图的动态阈值作为判断标准
                        const float MATCH_THRESHOLD = front_result.match_threshold;
                        const char *match_result;
                        const char *match_symbol;
                        
                        if (weighted_conf >= MATCH_THRESHOLD) {
                            match_result = "MATCH - Same Asset";
                            match_symbol = "✅";
                        } else {
                            match_result = "NO MATCH - Different Asset";
                            match_symbol = "❌";
                        }
                        
                        // ✅ 输出详细的盘点结果（包括混合相似度）
                        char result_msg[768];
                        snprintf(result_msg, sizeof(result_msg),
                                 "\r\n========== INVENTORY RESULT (OPTIMIZED) ==========\r\n"
                                 "  [FRONT VIEW]\r\n"
                                 "    Cosine:      %.4f\r\n"
                                 "    Euclidean:   %.4f\r\n"
                                 "    Mixed:       %.4f\r\n"
                                 "    Confidence:  %.4f (×0.5)\r\n"
                                 "  [SIDE VIEW]\r\n"
                                 "    Cosine:      %.4f\r\n"
                                 "    Euclidean:   %.4f\r\n"
                                 "    Mixed:       %.4f\r\n"
                                 "    Confidence:  %.4f (×0.3)\r\n"
                                 "  [TOP VIEW]\r\n"
                                 "    Cosine:      %.4f\r\n"
                                 "    Euclidean:   %.4f\r\n"
                                 "    Mixed:       %.4f\r\n"
                                 "    Confidence:  %.4f (×0.2)\r\n"
                                 "  ------------------------------------------------\r\n"
                                 "  Weighted Confidence: %.4f\r\n"
                                 "  Dynamic Threshold:   %.2f\r\n"
                                 "  %s %s\r\n"
                                 "  MAC: %s\r\n"
                                 "  Camera: POWER OFF\r\n"
                                 "===================================================\r\n",
                                 front_result.cosine_similarity, front_result.euclidean_similarity, 
                                 front_result.mixed_similarity, front_result.confidence,
                                 side_result.cosine_similarity, side_result.euclidean_similarity,
                                 side_result.mixed_similarity, side_result.confidence,
                                 top_result.cosine_similarity, top_result.euclidean_similarity,
                                 top_result.mixed_similarity, top_result.confidence,
                                 weighted_conf, MATCH_THRESHOLD, match_symbol, match_result, msg.mac);
                        uart_write_bytes(UART_NUM, (const char *)result_msg, strlen(result_msg));
                        
                        // LED指示：摄像头关闭 - 红色常亮
                        extern void led_camera_off(void);
                        led_camera_off();
                        
                        // 关闭摄像头电源
                        g_camera_power_on = false;
                        camera_module_deinit();
                        
                        // 重置状态，返回主菜单
                        g_camera_state = CAM_STATE_WAITING_MAC;
                        g_view_state = VIEW_NONE;
                        g_inventory_state = INVENTORY_IDLE;
                        g_is_inventory_mode = false;  // ✅ 重置模式标志
                        
                        // 显示主菜单
                        extern void show_main_menu(void);
                        show_main_menu();
                        
                        free(ref_record);
                    }
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown command: %d", msg.cmd);
                    break;
            }
        } else {
            // 超时，复位看门狗
            SAFE_WDT_RESET();
        }
    }
}

// 存储管理任务
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
                        ESP_LOGI(TAG, "Storage initialized successfully");
                    } else {
                        ESP_LOGE(TAG, "Storage initialization failed");
                    }
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unknown storage command: %d", msg.cmd);
                    break;
            }
        } else {
            // 超时，复位看门狗
            SAFE_WDT_RESET();
        }
    }
}

// ========== 主程序入口 ==========
void app_main(void)
{
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 初始化UART
    init_uart();
    
    // 初始化LED指示灯
    led_indicator_init();
    led_set_color(255, 0, 0);  // 开机红色指示灯 (R=255, G=0, B=0)
    
    // 创建队列
    xSystemQueue = xQueueCreate(10, sizeof(system_msg_t));
    xStorageQueue = xQueueCreate(5, sizeof(system_msg_t));
    
    // 显示主菜单
    printf("\n");
    printf("========== MAIN MENU ==========\r\n");
    printf("  r - Register new asset\r\n");
    printf("  c - Inventory existing asset\r\n");
    printf("  d - Delete asset\r\n");
    printf("  l - List all assets\r\n");
    printf("  i - System information\r\n");
    printf("  help/? - Show this menu\r\n");
    printf("================================\r\n");
    printf("[GUIDE] Please select an option: ");
    fflush(stdout);
    
    ESP_LOGI(TAG, "[SYSTEM] ESP32-CAM AI System Ready");
    
    // 创建任务
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(camera_ai_task, "camera_ai_task", 8192, NULL, 5, NULL);
    xTaskCreate(storage_task, "storage_task", 4096, NULL, 4, NULL);
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}