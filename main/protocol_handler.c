n#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>  // 用于目录操作 opendir, readdir, closedir
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"  // 看门狗复位
#include "driver/uart.h"
#include "driver/gpio.h"   // GPIO引脚定义
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "protocol_handler.h"
#include "camera_module.h"
#include "storage_module.h"
#include "ai_module.h"
#include "asset_manager.h"
#include "feature_processor.h"
#include "similarity_matcher.h"
#include "blur_detection.h"
#include "led_indicator.h"
#include "main.h"

static const char *TAG = "protocol_handler";

// ========== WS63 UART 配置（使用main.h中定义的值，避免冲突）==========
#define WS63_UART_NUM       UART_NUM_1
// GPIO引脚已在main.h中定义为 WS63_UART_TX_PIN (17) 和 WS63_UART_RX_PIN (18)
// 这里使用main.h中的宏定义
#define WS63_UART_BAUD      115200
// 缓冲区大小已在main.h中定义为 WS63_UART_BUF_SIZE (1024)

// ========== WS63 状态机 ==========
typedef enum {
    WS63_STATE_IDLE,              // 空闲，等待命令
    WS63_STATE_INITIALIZING,      // 收到register/inventory/outbound，正在初始化硬件
    WS63_STATE_WAITING_CAPTURE,   // 初始化完成，等待capture命令
    WS63_STATE_CAPTURING,         // 正在执行拍摄+推理
    WS63_STATE_FINALIZING         // 最后一个view完成，正在做最终保存/匹配
} ws63_state_t;

// ========== 全局变量 ==========
static ws63_state_t g_ws63_state = WS63_STATE_IDLE;
static ws63_cmd_t g_ws63_current_task = CMD_UNKNOWN;
static char g_ws63_mac[MAC_ADDR_LEN + 1] = {0};
static char g_ws63_item_name[128] = {0};
static char g_ws63_storage_area = 'A';
static uint32_t g_ws63_quantity = 0;
static uint32_t g_ws63_remove_qty = 0;
static uint32_t g_ws63_original_qty = 0;
static int g_ws63_total_views = 0;       // 3 (register/inventory) or 1 (outbound)
static int g_ws63_captured_views = 0;    // 已完成的视图数
static float g_ws63_front_feature[FEATURE_VEC_SIZE] = {0};
static float g_ws63_side_feature[FEATURE_VEC_SIZE] = {0};
static float g_ws63_top_feature[FEATURE_VEC_SIZE] = {0};

// ========== 内部函数声明 ==========
static esp_err_t ws63_uart_init(void);
static esp_err_t ws63_send_json_raw(const char *json_str);
static esp_err_t ws63_handle_register(cJSON *json_obj);
static esp_err_t ws63_handle_inventory(cJSON *json_obj);
static esp_err_t ws63_handle_outbound(cJSON *json_obj);
static esp_err_t ws63_handle_capture(cJSON *json_obj);
static esp_err_t ws63_handle_delete(cJSON *json_obj);
static esp_err_t ws63_handle_cancel(void);
static esp_err_t ws63_handle_list_assets(void);
static esp_err_t ws63_handle_get_asset(cJSON *json_obj);
static esp_err_t ws63_handle_sys_info(void);
static esp_err_t ws63_handle_ping(void);
static esp_err_t ws63_validate_mac(const char *mac);
static ws63_cmd_t ws63_parse_cmd(const char *cmd_str);
static const char* ws63_get_state_string(void);
static esp_err_t ws63_capture_and_process(capture_view_t view);
static esp_err_t ws63_finalize_task(void);
static void ws63_reset_state(void);

// ========== 错误码字符串映射 ==========
static const char* error_strings[] = {
    "INVALID_JSON",
    "UNKNOWN_CMD",
    "MISSING_FIELD",
    "INVALID_MAC",
    "INVALID_FIELD",
    "ASSET_NOT_FOUND",
    "ASSET_ALREADY_EXISTS",
    "STORAGE_NOT_READY",
    "CAMERA_FAIL",
    "AI_MODEL_FAIL",
    "CAPTURE_FAIL",
    "BLUR_DETECTED",
    "INFERENCE_FAIL",
    "SAVE_FAIL",
    "INTERNAL_ERROR",
    "NOT_INITIALIZED",
    "TASK_BUSY"
};

// ========== 命令字符串映射 ==========
static const char* cmd_strings[] = {
    "register",
    "inventory",
    "outbound",
    "capture",
    "delete",
    "cancel",
    "list_assets",
    "get_asset",
    "sys_info",
    "ping",
    "unknown"
};

// ========== 视图字符串映射 ==========
static const char* view_strings[] = {
    "none",
    "front",
    "side",
    "top"
};

// ========== 公共函数实现 ==========

/**
 * @brief 初始化协议处理器
 */
void protocol_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing WS63 protocol handler");
    
    // 初始化UART1
    esp_err_t ret = ws63_uart_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WS63 UART: %s", esp_err_to_name(ret));
        return;
    }
    
    // 初始化特征处理器
    feature_processor_config_t config = {
        .temperature_scale = 0.8f,
        .num_frames = 3,
        .enable_batch_norm = true,
        .batch_norm_momentum = 0.99f
    };
    if (!feature_processor_init(&config)) {
        ESP_LOGW(TAG, "Feature processor initialization failed");
    }
    
    // 创建WS63 UART接收任务
    xTaskCreate(ws63_recv_task, "ws63_recv_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "WS63 protocol handler initialized");
}

/**
 * @brief 处理接收到的JSON命令
 */
esp_err_t protocol_handle_command(const char *json_str)
{
    if (json_str == NULL || strlen(json_str) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing JSON command: %s", json_str);
    
    // 解析JSON
    cJSON *json_obj = cJSON_Parse(json_str);
    if (json_obj == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr);
        }
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取命令字段
    cJSON *cmd_item = cJSON_GetObjectItem(json_obj, "cmd");
    if (cmd_item == NULL || !cJSON_IsString(cmd_item)) {
        ESP_LOGE(TAG, "Missing or invalid 'cmd' field");
        cJSON_Delete(json_obj);
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *cmd_str = cmd_item->valuestring;
    ws63_cmd_t cmd = ws63_parse_cmd(cmd_str);
    
    esp_err_t ret = ESP_OK;
    
    // 根据命令类型处理
    switch (cmd) {
        case CMD_REGISTER:
            ret = ws63_handle_register(json_obj);
            break;
        case CMD_INVENTORY:
            ret = ws63_handle_inventory(json_obj);
            break;
        case CMD_OUTBOUND:
            ret = ws63_handle_outbound(json_obj);
            break;
        case CMD_CAPTURE:
            ret = ws63_handle_capture(json_obj);
            break;
        case CMD_DELETE:
            ret = ws63_handle_delete(json_obj);
            break;
        case CMD_CANCEL:
            ret = ws63_handle_cancel();
            break;
        case CMD_LIST_ASSETS:
            ret = ws63_handle_list_assets();
            break;
        case CMD_GET_ASSET:
            ret = ws63_handle_get_asset(json_obj);
            break;
        case CMD_SYS_INFO:
            ret = ws63_handle_sys_info();
            break;
        case CMD_PING:
            ret = ws63_handle_ping();
            break;
        default:
            ESP_LOGE(TAG, "Unknown command: %s", cmd_str);
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }
    
    cJSON_Delete(json_obj);
    return ret;
}

/**
 * @brief 发送JSON响应到WS63
 */
esp_err_t protocol_send_response(const char *json_str)
{
    return ws63_send_json_raw(json_str);
}

/**
 * @brief 生成错误响应JSON
 */
esp_err_t protocol_generate_error_response(ws63_error_t error_code, const char *error_msg, 
                                          char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 128) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "error");
    cJSON_AddStringToObject(json_obj, "code", protocol_get_error_string(error_code));
    cJSON_AddStringToObject(json_obj, "msg", error_msg);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 生成拍摄进度响应JSON
 */
esp_err_t protocol_generate_capture_progress(const char *mac, capture_view_t view, 
                                            const char *step, const char *status,
                                            float blur_score, char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 256) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "capture_progress");
    cJSON_AddStringToObject(json_obj, "mac", mac);
    cJSON_AddStringToObject(json_obj, "view", protocol_get_view_string(view));
    cJSON_AddStringToObject(json_obj, "step", step);
    cJSON_AddStringToObject(json_obj, "status", status);
    cJSON_AddNumberToObject(json_obj, "blur_score", blur_score);
    cJSON_AddNumberToObject(json_obj, "feature_size", FEATURE_VEC_SIZE);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 生成任务完成响应JSON
 */
esp_err_t protocol_generate_task_done(ws63_cmd_t task, const char *mac, const char *result,
                                     char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 256) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "task_done");
    cJSON_AddStringToObject(json_obj, "task", protocol_get_cmd_string(task));
    cJSON_AddStringToObject(json_obj, "mac", mac);
    cJSON_AddStringToObject(json_obj, "result", result);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 生成资产列表响应JSON
 */
esp_err_t protocol_generate_asset_list(int count, char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "asset_list");
    cJSON_AddNumberToObject(json_obj, "count", count);
    
    // 创建资产数组
    cJSON *assets_array = cJSON_CreateArray();
    if (assets_array == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    // 如果count > 0，尝试加载资产列表
    if (count > 0) {
        // 初始化存储
        extern bool storage_module_init(void);
        if (storage_module_init()) {
            // 打开资产目录
            DIR *dir = opendir("/sdcard/assets");
            if (dir != NULL) {
                struct dirent *entry;
                int asset_count = 0;
                
                while ((entry = readdir(dir)) != NULL && asset_count < count) {
                    // 只处理.dat文件
                    if (strstr(entry->d_name, ".dat") != NULL) {
                        // 将文件名中的'_'转换回':'显示
                        char mac_display[MAC_ADDR_LEN + 1];
                        strncpy(mac_display, entry->d_name, sizeof(mac_display) - 1);
                        mac_display[sizeof(mac_display) - 1] = '\0';
                        
                        // 移除扩展名
                        char *dot = strstr(mac_display, ".dat");
                        if (dot) {
                            *dot = '\0';
                        }
                        
                        // 将'_'转换回':'
                        for (int i = 0; i < strlen(mac_display); i++) {
                            if (mac_display[i] == '_') {
                                mac_display[i] = ':';
                            }
                        }
                        
                        // 创建资产对象
                        cJSON *asset_obj = cJSON_CreateObject();
                        if (asset_obj != NULL) {
                            cJSON_AddStringToObject(asset_obj, "mac", mac_display);
                            
                            // 尝试加载资产记录获取更多信息
                            asset_record_t record;
                            if (asset_load(mac_display, &record) == ESP_OK) {
                                cJSON_AddStringToObject(asset_obj, "item_name", record.item_name);
                                cJSON_AddStringToObject(asset_obj, "storage_area", (char[2]){record.storage_area, '\0'});
                                cJSON_AddNumberToObject(asset_obj, "quantity", record.quantity);
                            }
                            
                            cJSON_AddItemToArray(assets_array, asset_obj);
                            asset_count++;
                        }
                    }
                }
                closedir(dir);
            }
        }
    }
    
    cJSON_AddItemToObject(json_obj, "assets", assets_array);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 生成系统信息响应JSON
 */
esp_err_t protocol_generate_sys_info(uint32_t free_heap, uint32_t min_free_heap,
                                    bool camera_ready, bool storage_ready,
                                    uint32_t storage_total_mb, uint32_t storage_free_mb,
                                    const char *current_task, char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 512) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "sys_info");
    cJSON_AddNumberToObject(json_obj, "free_heap", free_heap);
    cJSON_AddNumberToObject(json_obj, "min_free_heap", min_free_heap);
    cJSON_AddBoolToObject(json_obj, "camera_ready", camera_ready);
    cJSON_AddBoolToObject(json_obj, "storage_ready", storage_ready);
    cJSON_AddStringToObject(json_obj, "storage_mode", "sd_card");
    cJSON_AddNumberToObject(json_obj, "storage_total_mb", storage_total_mb);
    cJSON_AddNumberToObject(json_obj, "storage_free_mb", storage_free_mb);
    cJSON_AddStringToObject(json_obj, "current_task", current_task);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 生成心跳响应JSON
 */
esp_err_t protocol_generate_pong(bool camera_ready, bool storage_ready,
                                uint32_t free_heap, const char *current_task,
                                char *json_buf, size_t buf_size)
{
    if (json_buf == NULL || buf_size < 256) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *json_obj = cJSON_CreateObject();
    if (json_obj == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj, "type", "pong");
    cJSON_AddBoolToObject(json_obj, "camera_ready", camera_ready);
    cJSON_AddBoolToObject(json_obj, "storage_ready", storage_ready);
    cJSON_AddNumberToObject(json_obj, "free_heap", free_heap);
    cJSON_AddStringToObject(json_obj, "current_task", current_task);
    
    char *json_str = cJSON_PrintUnformatted(json_obj);
    if (json_str == NULL) {
        cJSON_Delete(json_obj);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(json_buf, json_str, buf_size - 1);
    json_buf[buf_size - 1] = '\0';
    
    free(json_str);
    cJSON_Delete(json_obj);
    
    return ESP_OK;
}

/**
 * @brief 获取错误码对应的字符串描述
 */
const char *protocol_get_error_string(ws63_error_t error_code)
{
    if (error_code < 0 || error_code >= sizeof(error_strings) / sizeof(error_strings[0])) {
        return "UNKNOWN_ERROR";
    }
    return error_strings[error_code];
}

/**
 * @brief 获取命令类型对应的字符串
 */
const char *protocol_get_cmd_string(ws63_cmd_t cmd)
{
    if (cmd < 0 || cmd >= sizeof(cmd_strings) / sizeof(cmd_strings[0])) {
        return "unknown";
    }
    return cmd_strings[cmd];
}

/**
 * @brief 获取视图类型对应的字符串
 */
const char *protocol_get_view_string(capture_view_t view)
{
    if (view < 0 || view >= sizeof(view_strings) / sizeof(view_strings[0])) {
        return "none";
    }
    return view_strings[view];
}

// ========== 内部函数实现 ==========

/**
 * @brief 初始化WS63 UART
 */
static esp_err_t ws63_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = WS63_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    esp_err_t ret = uart_param_config(WS63_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_set_pin(WS63_UART_NUM, WS63_UART_TX_PIN, WS63_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_driver_install(WS63_UART_NUM, WS63_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WS63 UART initialized (TX: GPIO%d, RX: GPIO%d, Baud: %d)",
             WS63_UART_TX_PIN, WS63_UART_RX_PIN, WS63_UART_BAUD);
    
    return ESP_OK;
}

/**
 * @brief 发送JSON字符串到WS63 UART
 */
static esp_err_t ws63_send_json_raw(const char *json_str)
{
    if (json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t len = strlen(json_str);
    char *msg_with_newline = malloc(len + 2); // +1 for newline, +1 for null terminator
    if (msg_with_newline == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    snprintf(msg_with_newline, len + 2, "%s\n", json_str);
    
    int bytes_written = uart_write_bytes(WS63_UART_NUM, msg_with_newline, len + 1);
    free(msg_with_newline);
    
    if (bytes_written != len + 1) {
        ESP_LOGE(TAG, "Failed to send JSON to WS63 (expected %d, wrote %d)", len + 1, bytes_written);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Sent to WS63: %s", json_str);
    return ESP_OK;
}

/**
 * @brief 验证MAC地址格式
 */
static esp_err_t ws63_validate_mac(const char *mac)
{
    if (mac == NULL || strlen(mac) != MAC_ADDR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 格式: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < MAC_ADDR_LEN; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (mac[i] != ':') {
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            char c = mac[i];
            if (!((c >= '0' && c <= '9') || 
                  (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f'))) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    
    return ESP_OK;
}

/**
 * @brief 解析命令字符串
 */
static ws63_cmd_t ws63_parse_cmd(const char *cmd_str)
{
    if (cmd_str == NULL) {
        return CMD_UNKNOWN;
    }
    
    if (strcmp(cmd_str, "register") == 0) return CMD_REGISTER;
    if (strcmp(cmd_str, "inventory") == 0) return CMD_INVENTORY;
    if (strcmp(cmd_str, "outbound") == 0) return CMD_OUTBOUND;
    if (strcmp(cmd_str, "capture") == 0) return CMD_CAPTURE;
    if (strcmp(cmd_str, "delete") == 0) return CMD_DELETE;
    if (strcmp(cmd_str, "cancel") == 0) return CMD_CANCEL;
    if (strcmp(cmd_str, "list_assets") == 0) return CMD_LIST_ASSETS;
    if (strcmp(cmd_str, "get_asset") == 0) return CMD_GET_ASSET;
    if (strcmp(cmd_str, "sys_info") == 0) return CMD_SYS_INFO;
    if (strcmp(cmd_str, "ping") == 0) return CMD_PING;
    
    return CMD_UNKNOWN;
}

/**
 * @brief 获取当前状态字符串
 */
static const char* ws63_get_state_string(void)
{
    switch (g_ws63_state) {
        case WS63_STATE_IDLE: return "idle";
        case WS63_STATE_INITIALIZING: return "initializing";
        case WS63_STATE_WAITING_CAPTURE: return "waiting_capture";
        case WS63_STATE_CAPTURING: return "capturing";
        case WS63_STATE_FINALIZING: return "finalizing";
        default: return "unknown";
    }
}

/**
 * @brief 处理入库注册命令
 */
static esp_err_t ws63_handle_register(cJSON *json_obj)
{
    // 检查任务忙状态
    if (g_ws63_state != WS63_STATE_IDLE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_TASK_BUSY, "Another task is in progress", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 验证必填字段
    cJSON *mac_item = cJSON_GetObjectItem(json_obj, "mac");
    cJSON *item_name_item = cJSON_GetObjectItem(json_obj, "item_name");
    cJSON *storage_area_item = cJSON_GetObjectItem(json_obj, "storage_area");
    cJSON *quantity_item = cJSON_GetObjectItem(json_obj, "quantity");
    
    if (!mac_item || !cJSON_IsString(mac_item) ||
        !item_name_item || !cJSON_IsString(item_name_item) ||
        !storage_area_item || !cJSON_IsString(storage_area_item) ||
        !quantity_item || !cJSON_IsNumber(quantity_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing required fields", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证MAC地址格式
    const char *mac = mac_item->valuestring;
    if (ws63_validate_mac(mac) != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_MAC, "Invalid MAC address format", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证物品名称长度
    const char *item_name = item_name_item->valuestring;
    if (strlen(item_name) == 0 || strlen(item_name) >= 128) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_FIELD, "Item name must be 1-127 characters", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证存放区域
    const char *storage_area_str = storage_area_item->valuestring;
    if (strlen(storage_area_str) != 1 || !isalpha((unsigned char)storage_area_str[0])) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_FIELD, "Storage area must be a single letter A-Z", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证数量
    int quantity = quantity_item->valueint;
    if (quantity <= 0) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_FIELD, "Quantity must be positive", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 保存参数到全局变量
    strncpy(g_ws63_mac, mac, sizeof(g_ws63_mac) - 1);
    strncpy(g_ws63_item_name, item_name, sizeof(g_ws63_item_name) - 1);
    g_ws63_storage_area = toupper((unsigned char)storage_area_str[0]);
    g_ws63_quantity = quantity;
    g_ws63_current_task = CMD_REGISTER;
    g_ws63_total_views = 3;  // 注册需要3个视图
    g_ws63_captured_views = 0;
    
    // 更新状态
    g_ws63_state = WS63_STATE_INITIALIZING;
    
    // 初始化AI模块
    if (!ai_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_AI_MODEL_FAIL, "AI model initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 初始化存储
    extern bool storage_module_init(void);
    if (!storage_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_STORAGE_NOT_READY, "Storage initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 初始化摄像头
    if (!camera_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_CAMERA_FAIL, "Camera initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 发送初始化完成响应
    char progress_buf[256];
    protocol_generate_capture_progress(g_ws63_mac, VIEW_NONE, "0/3", "ready", 0.0f, 
                                      progress_buf, sizeof(progress_buf));
    ws63_send_json_raw(progress_buf);
    
    // 更新状态为等待拍摄
    g_ws63_state = WS63_STATE_WAITING_CAPTURE;
    
    ESP_LOGI(TAG, "Register command accepted: MAC=%s, Item=%s, Area=%c, Qty=%lu",
             g_ws63_mac, g_ws63_item_name, g_ws63_storage_area, (unsigned long)g_ws63_quantity);
    
    return ESP_OK;
}

/**
 * @brief 处理盘点比对命令
 */
static esp_err_t ws63_handle_inventory(cJSON *json_obj)
{
    // 检查任务忙状态
    if (g_ws63_state != WS63_STATE_IDLE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_TASK_BUSY, "Another task is in progress", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 验证必填字段
    cJSON *mac_item = cJSON_GetObjectItem(json_obj, "mac");
    if (!mac_item || !cJSON_IsString(mac_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing 'mac' field", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证MAC地址格式
    const char *mac = mac_item->valuestring;
    if (ws63_validate_mac(mac) != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_MAC, "Invalid MAC address format", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查资产是否存在
    asset_record_t ref_record;
    esp_err_t ret = asset_load(mac, &ref_record);
    if (ret != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_ASSET_NOT_FOUND, "Asset not found", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 保存参数到全局变量
    strncpy(g_ws63_mac, mac, sizeof(g_ws63_mac) - 1);
    strncpy(g_ws63_item_name, ref_record.item_name, sizeof(g_ws63_item_name) - 1);
    g_ws63_storage_area = ref_record.storage_area;
    g_ws63_quantity = ref_record.quantity;
    g_ws63_current_task = CMD_INVENTORY;
    g_ws63_total_views = 3;  // 盘点需要3个视图
    g_ws63_captured_views = 0;
    
    // 保存参考特征
    memcpy(g_ws63_front_feature, ref_record.front_feature, sizeof(g_ws63_front_feature));
    memcpy(g_ws63_side_feature, ref_record.side_feature, sizeof(g_ws63_side_feature));
    memcpy(g_ws63_top_feature, ref_record.top_feature, sizeof(g_ws63_top_feature));
    
    // 更新状态
    g_ws63_state = WS63_STATE_INITIALIZING;
    
    // 初始化AI模块
    if (!ai_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_AI_MODEL_FAIL, "AI model initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 初始化摄像头
    if (!camera_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_CAMERA_FAIL, "Camera initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 发送初始化完成响应
    char progress_buf[256];
    protocol_generate_capture_progress(g_ws63_mac, VIEW_NONE, "0/3", "ready", 0.0f, 
                                      progress_buf, sizeof(progress_buf));
    ws63_send_json_raw(progress_buf);
    
    // 更新状态为等待拍摄
    g_ws63_state = WS63_STATE_WAITING_CAPTURE;
    
    ESP_LOGI(TAG, "Inventory command accepted: MAC=%s", g_ws63_mac);
    
    return ESP_OK;
}

/**
 * @brief 处理出库核验命令
 */
static esp_err_t ws63_handle_outbound(cJSON *json_obj)
{
    // 检查任务忙状态
    if (g_ws63_state != WS63_STATE_IDLE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_TASK_BUSY, "Another task is in progress", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 验证必填字段
    cJSON *mac_item = cJSON_GetObjectItem(json_obj, "mac");
    cJSON *remove_qty_item = cJSON_GetObjectItem(json_obj, "remove_qty");
    
    if (!mac_item || !cJSON_IsString(mac_item) ||
        !remove_qty_item || !cJSON_IsNumber(remove_qty_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing required fields", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证MAC地址格式
    const char *mac = mac_item->valuestring;
    if (ws63_validate_mac(mac) != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_MAC, "Invalid MAC address format", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证出库数量
    int remove_qty = remove_qty_item->valueint;
    if (remove_qty <= 0) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_FIELD, "Remove quantity must be positive", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查资产是否存在
    asset_record_t ref_record;
    esp_err_t ret = asset_load(mac, &ref_record);
    if (ret != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_ASSET_NOT_FOUND, "Asset not found", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 保存参数到全局变量
    strncpy(g_ws63_mac, mac, sizeof(g_ws63_mac) - 1);
    strncpy(g_ws63_item_name, ref_record.item_name, sizeof(g_ws63_item_name) - 1);
    g_ws63_storage_area = ref_record.storage_area;
    g_ws63_quantity = ref_record.quantity;
    g_ws63_remove_qty = remove_qty;
    g_ws63_original_qty = ref_record.quantity;
    g_ws63_current_task = CMD_OUTBOUND;
    g_ws63_total_views = 1;  // 出库只需要1个视图
    g_ws63_captured_views = 0;
    
    // 保存参考特征（仅正面）
    memcpy(g_ws63_front_feature, ref_record.front_feature, sizeof(g_ws63_front_feature));
    
    // 更新状态
    g_ws63_state = WS63_STATE_INITIALIZING;
    
    // 初始化AI模块
    if (!ai_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_AI_MODEL_FAIL, "AI model initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 初始化摄像头
    if (!camera_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_CAMERA_FAIL, "Camera initialization failed", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ws63_reset_state();
        return ESP_FAIL;
    }
    
    // 发送初始化完成响应
    char progress_buf[256];
    protocol_generate_capture_progress(g_ws63_mac, VIEW_NONE, "0/1", "ready", 0.0f, 
                                      progress_buf, sizeof(progress_buf));
    ws63_send_json_raw(progress_buf);
    
    // 更新状态为等待拍摄
    g_ws63_state = WS63_STATE_WAITING_CAPTURE;
    
    ESP_LOGI(TAG, "Outbound command accepted: MAC=%s, Remove=%lu, Original=%lu",
             g_ws63_mac, (unsigned long)g_ws63_remove_qty, (unsigned long)g_ws63_original_qty);
    
    return ESP_OK;
}

/**
 * @brief 处理单步拍摄视图命令
 */
static esp_err_t ws63_handle_capture(cJSON *json_obj)
{
    // 检查状态
    if (g_ws63_state != WS63_STATE_WAITING_CAPTURE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_NOT_INITIALIZED, 
                                        "Hardware not initialized or not in capture state", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 验证必填字段
    cJSON *view_item = cJSON_GetObjectItem(json_obj, "view");
    if (!view_item || !cJSON_IsString(view_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing 'view' field", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 解析视图类型
    const char *view_str = view_item->valuestring;
    capture_view_t view = VIEW_NONE;
    
    if (strcmp(view_str, "front") == 0) view = VIEW_FRONT;
    else if (strcmp(view_str, "side") == 0) view = VIEW_SIDE;
    else if (strcmp(view_str, "top") == 0) view = VIEW_TOP;
    else {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_FIELD, 
                                        "View must be 'front', 'side', or 'top'", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查视图顺序（仅注册和盘点模式需要顺序）
    if (g_ws63_current_task == CMD_REGISTER || g_ws63_current_task == CMD_INVENTORY) {
        if (g_ws63_captured_views == 0 && view != VIEW_FRONT) {
            char error_buf[256];
            protocol_generate_error_response(ERR_INVALID_FIELD, 
                                            "First view must be 'front'", 
                                            error_buf, sizeof(error_buf));
            ws63_send_json_raw(error_buf);
            return ESP_ERR_INVALID_ARG;
        }
        if (g_ws63_captured_views == 1 && view != VIEW_SIDE) {
            char error_buf[256];
            protocol_generate_error_response(ERR_INVALID_FIELD, 
                                            "Second view must be 'side'", 
                                            error_buf, sizeof(error_buf));
            ws63_send_json_raw(error_buf);
            return ESP_ERR_INVALID_ARG;
        }
        if (g_ws63_captured_views == 2 && view != VIEW_TOP) {
            char error_buf[256];
            protocol_generate_error_response(ERR_INVALID_FIELD, 
                                            "Third view must be 'top'", 
                                            error_buf, sizeof(error_buf));
            ws63_send_json_raw(error_buf);
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    // 更新状态
    g_ws63_state = WS63_STATE_CAPTURING;
    
    // 执行拍摄和推理
    esp_err_t ret = ws63_capture_and_process(view);
    if (ret != ESP_OK) {
        // 错误已在ws63_capture_and_process中处理
        return ret;
    }
    
    // 更新已拍摄视图计数
    g_ws63_captured_views++;
    
    // 检查是否所有视图都已完成
    if (g_ws63_captured_views >= g_ws63_total_views) {
        // 所有视图完成，进入最终处理阶段
        g_ws63_state = WS63_STATE_FINALIZING;
        ret = ws63_finalize_task();
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        // 还有更多视图需要拍摄，返回等待状态
        g_ws63_state = WS63_STATE_WAITING_CAPTURE;
    }
    
    return ESP_OK;
}

/**
 * @brief 处理删除资产命令
 */
static esp_err_t ws63_handle_delete(cJSON *json_obj)
{
    // 检查任务忙状态（删除命令可以在任何状态下执行，除了正在执行其他任务）
    if (g_ws63_state != WS63_STATE_IDLE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_TASK_BUSY, "Another task is in progress", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 验证必填字段
    cJSON *mac_item = cJSON_GetObjectItem(json_obj, "mac");
    if (!mac_item || !cJSON_IsString(mac_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing 'mac' field", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证MAC地址格式
    const char *mac = mac_item->valuestring;
    if (ws63_validate_mac(mac) != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_MAC, "Invalid MAC address format", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 执行删除
    esp_err_t ret = asset_delete(mac);
    
    // 发送响应
    char response_buf[256];
    if (ret == ESP_OK) {
        protocol_generate_task_done(CMD_DELETE, mac, "success", response_buf, sizeof(response_buf));
    } else if (ret == ESP_ERR_NOT_FOUND) {
        protocol_generate_task_done(CMD_DELETE, mac, "failed", response_buf, sizeof(response_buf));
    } else {
        protocol_generate_error_response(ERR_INTERNAL_ERROR, "Failed to delete asset", 
                                        response_buf, sizeof(response_buf));
    }
    
    ws63_send_json_raw(response_buf);
    
    ESP_LOGI(TAG, "Delete command processed: MAC=%s, Result=%s", mac, 
             ret == ESP_OK ? "success" : "failed");
    
    return ret;
}

/**
 * @brief 处理取消当前任务命令
 */
static esp_err_t ws63_handle_cancel(void)
{
    // 检查是否有任务在执行
    if (g_ws63_state == WS63_STATE_IDLE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_STATE, "No task to cancel", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 保存当前任务信息用于响应
    ws63_cmd_t cancelled_task = g_ws63_current_task;
    char cancelled_mac[MAC_ADDR_LEN + 1];
    strncpy(cancelled_mac, g_ws63_mac, sizeof(cancelled_mac));
    
    // 重置状态
    ws63_reset_state();
    
    // 关闭摄像头
    camera_module_deinit();
    
    // 发送取消响应
    char response_buf[256];
    protocol_generate_task_done(cancelled_task, cancelled_mac, "cancelled", 
                               response_buf, sizeof(response_buf));
    ws63_send_json_raw(response_buf);
    
    ESP_LOGI(TAG, "Task cancelled: %s, MAC=%s", 
             protocol_get_cmd_string(cancelled_task), cancelled_mac);
    
    return ESP_OK;
}

/**
 * @brief 处理查询资产列表命令
 */
static esp_err_t ws63_handle_list_assets(void)
{
    // 初始化存储（如果未初始化）
    extern bool storage_module_init(void);
    if (!storage_module_init()) {
        char error_buf[256];
        protocol_generate_error_response(ERR_STORAGE_NOT_READY, "Storage not ready", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_FAIL;
    }
    
    // 获取资产数量
    int count = 0;
    esp_err_t ret = asset_list_all(&count);
    if (ret != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INTERNAL_ERROR, "Failed to list assets", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ret;
    }
    
    // 生成响应
    char response_buf[1024];  // 可能需要更大的缓冲区
    protocol_generate_asset_list(count, response_buf, sizeof(response_buf));
    ws63_send_json_raw(response_buf);
    
    ESP_LOGI(TAG, "List assets command processed: %d assets found", count);
    
    return ESP_OK;
}

/**
 * @brief 处理查询单个资产详情命令
 */
static esp_err_t ws63_handle_get_asset(cJSON *json_obj)
{
    // 验证必填字段
    cJSON *mac_item = cJSON_GetObjectItem(json_obj, "mac");
    if (!mac_item || !cJSON_IsString(mac_item)) {
        char error_buf[256];
        protocol_generate_error_response(ERR_MISSING_FIELD, "Missing 'mac' field", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 验证MAC地址格式
    const char *mac = mac_item->valuestring;
    if (ws63_validate_mac(mac) != ESP_OK) {
        char error_buf[256];
        protocol_generate_error_response(ERR_INVALID_MAC, "Invalid MAC address format", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 加载资产
    asset_record_t record;
    esp_err_t ret = asset_load(mac, &record);
    
    // 生成响应
    cJSON *json_obj_resp = cJSON_CreateObject();
    if (json_obj_resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(json_obj_resp, "type", "asset_detail");
    cJSON_AddStringToObject(json_obj_resp, "mac", mac);
    
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(json_obj_resp, "found", true);
        cJSON_AddStringToObject(json_obj_resp, "item_name", record.item_name);
        cJSON_AddStringToObject(json_obj_resp, "storage_area", (char[2]){record.storage_area, '\0'});
        cJSON_AddNumberToObject(json_obj_resp, "quantity", record.quantity);
    } else {
        cJSON_AddBoolToObject(json_obj_resp, "found", false);
    }
    
    char *json_str = cJSON_PrintUnformatted(json_obj_resp);
    if (json_str == NULL) {
        cJSON_Delete(json_obj_resp);
        return ESP_ERR_NO_MEM;
    }
    
    ws63_send_json_raw(json_str);
    free(json_str);
    cJSON_Delete(json_obj_resp);
    
    ESP_LOGI(TAG, "Get asset command processed: MAC=%s, Found=%s", 
             mac, ret == ESP_OK ? "true" : "false");
    
    return ESP_OK;
}

/**
 * @brief 处理查询系统信息命令
 */
static esp_err_t ws63_handle_sys_info(void)
{
    // 获取系统信息
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    
    // 获取存储信息
    uint64_t total_bytes = 0, used_bytes = 0, free_bytes = 0;
    esp_err_t storage_ret = asset_get_storage_info(&total_bytes, &used_bytes, &free_bytes);
    
    bool camera_ready = false;
    bool storage_ready = (storage_ret == ESP_OK);
    
    // 生成响应
    char response_buf[512];
    protocol_generate_sys_info(free_heap, min_free_heap,
                              camera_ready, storage_ready,
                              (uint32_t)(total_bytes / (1024 * 1024)),
                              (uint32_t)(free_bytes / (1024 * 1024)),
                              ws63_get_state_string(),
                              response_buf, sizeof(response_buf));
    ws63_send_json_raw(response_buf);
    
    ESP_LOGI(TAG, "System info command processed");
    
    return ESP_OK;
}

/**
 * @brief 处理心跳/状态检测命令
 */
static esp_err_t ws63_handle_ping(void)
{
    // 获取系统状态
    uint32_t free_heap = esp_get_free_heap_size();
    
    // 检查存储状态
    extern bool storage_module_init(void);
    bool storage_ready = storage_module_init();
    
    // 检查摄像头状态（简化检查）
    bool camera_ready = false;
    
    // 生成响应
    char response_buf[256];
    protocol_generate_pong(camera_ready, storage_ready, free_heap,
                          ws63_get_state_string(), response_buf, sizeof(response_buf));
    ws63_send_json_raw(response_buf);
    
    ESP_LOGD(TAG, "Ping command processed");
    
    return ESP_OK;
}

/**
 * @brief 执行拍摄和推理
 */
static esp_err_t ws63_capture_and_process(capture_view_t view)
{
    ESP_LOGI(TAG, "Capturing %s view for MAC=%s", protocol_get_view_string(view), g_ws63_mac);
    
    // 获取摄像头互斥锁
    extern SemaphoreHandle_t xCameraMutex;
    if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        char error_buf[256];
        protocol_generate_error_response(ERR_CAMERA_FAIL, "Camera mutex timeout", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = ESP_OK;
    float blur_score = 0.0f;
    const char *status = "ok";
    
    // 清空特征处理器缓冲区
    feature_processor_clear_buffer();
    
    // 拍摄3帧并处理
    int frames_captured = 0;
    const int NUM_FRAMES = 3;
    
    for (int i = 0; i < NUM_FRAMES; i++) {
        float single_frame[FEATURE_VEC_SIZE];
        
        // 捕获并处理单帧
        if (camera_module_capture_and_process(single_frame, FEATURE_VEC_SIZE)) {
            // 这里可以添加模糊检测
            // TODO: 实现模糊检测
            
            // 添加到特征处理器
            feature_processor_add_frame(single_frame, FEATURE_VEC_SIZE);
            frames_captured++;
            
            ESP_LOGD(TAG, "Frame %d/%d captured for %s view", i + 1, NUM_FRAMES, 
                     protocol_get_view_string(view));
        } else {
            ESP_LOGW(TAG, "Failed to capture frame %d/%d for %s view", i + 1, NUM_FRAMES,
                     protocol_get_view_string(view));
        }
    }
    
    // 释放摄像头互斥锁
    xSemaphoreGive(xCameraMutex);
    
    // 检查是否捕获到足够的帧
    if (frames_captured == 0) {
        char error_buf[256];
        protocol_generate_error_response(ERR_CAPTURE_FAIL, "Failed to capture any frames", 
                                        error_buf, sizeof(error_buf));
        ws63_send_json_raw(error_buf);
        ret = ESP_FAIL;
        goto cleanup;
    }
    
    // 获取融合后的特征
    float *feature_ptr = NULL;
    switch (view) {
        case VIEW_FRONT:
            feature_ptr = g_ws63_front_feature;
            break;
        case VIEW_SIDE:
            feature_ptr = g_ws63_side_feature;
            break;
        case VIEW_TOP:
            feature_ptr = g_ws63_top_feature;
            break;
        default:
            break;
    }
    
    if (feature_ptr != NULL) {
        if (!feature_processor_get_fused_feature(feature_ptr, FEATURE_VEC_SIZE)) {
            ESP_LOGW(TAG, "Failed to get fused feature for %s view", 
                     protocol_get_view_string(view));
        }
    }
    
    // 检查模糊度
    if (blur_score < 50.0f) {
        status = "blur_warning";
    }
    
    // 发送进度响应
    char step_str[16];
    snprintf(step_str, sizeof(step_str), "%d/%d", g_ws63_captured_views + 1, g_ws63_total_views);
    
    char progress_buf[256];
    protocol_generate_capture_progress(g_ws63_mac, view, step_str, status, blur_score,
                                      progress_buf, sizeof(progress_buf));
    ws63_send_json_raw(progress_buf);
    
    ESP_LOGI(TAG, "%s view captured: %s, Blur=%.1f, Status=%s",
             protocol_get_view_string(view), step_str, blur_score, status);
    
cleanup:
    return ret;
}

/**
 * @brief 最终化任务（保存/匹配）
 */
static esp_err_t ws63_finalize_task(void)
{
    ESP_LOGI(TAG, "Finalizing task: %s, MAC=%s", 
             protocol_get_cmd_string(g_ws63_current_task), g_ws63_mac);
    
    esp_err_t ret = ESP_OK;
    
    switch (g_ws63_current_task) {
        case CMD_REGISTER: {
            // 创建资产记录
            asset_record_t record;
            memset(&record, 0, sizeof(record));
            
            strncpy(record.mac_address, g_ws63_mac, sizeof(record.mac_address) - 1);
            strncpy(record.item_name, g_ws63_item_name, sizeof(record.item_name) - 1);
            record.storage_area = g_ws63_storage_area;
            record.quantity = g_ws63_quantity;
            memcpy(record.front_feature, g_ws63_front_feature, sizeof(record.front_feature));
            memcpy(record.side_feature, g_ws63_side_feature, sizeof(record.side_feature));
            memcpy(record.top_feature, g_ws63_top_feature, sizeof(record.top_feature));
            record.is_valid = true;
            
            // 保存资产
            bool is_overwrite = false;
            ret = asset_save(&record, &is_overwrite);
            
            // 生成响应
            char response_buf[512];
            if (ret == ESP_OK) {
                cJSON *json_obj = cJSON_CreateObject();
                if (json_obj) {
                    cJSON_AddStringToObject(json_obj, "type", "task_done");
                    cJSON_AddStringToObject(json_obj, "task", "register");
                    cJSON_AddStringToObject(json_obj, "mac", g_ws63_mac);
                    cJSON_AddStringToObject(json_obj, "result", "success");
                    cJSON_AddStringToObject(json_obj, "item_name", g_ws63_item_name);
                    cJSON_AddStringToObject(json_obj, "storage_area", (char[2]){g_ws63_storage_area, '\0'});
                    cJSON_AddNumberToObject(json_obj, "quantity", g_ws63_quantity);
                    cJSON_AddBoolToObject(json_obj, "is_overwrite", is_overwrite);
                    cJSON_AddNumberToObject(json_obj, "file_size_kb", 45); // 估算值
                    
                    char *json_str = cJSON_PrintUnformatted(json_obj);
                    if (json_str) {
                        strncpy(response_buf, json_str, sizeof(response_buf) - 1);
                        free(json_str);
                    }
                    cJSON_Delete(json_obj);
                }
            } else {
                protocol_generate_task_done(CMD_REGISTER, g_ws63_mac, "failed", 
                                           response_buf, sizeof(response_buf));
            }
            
            ws63_send_json_raw(response_buf);
            break;
        }
            
        case CMD_INVENTORY: {
            // 计算相似度
            similarity_result_t front_result = {0};
            similarity_result_t side_result = {0};
            similarity_result_t top_result = {0};
            
            // 加载参考特征
            asset_record_t ref_record;
            ret = asset_load(g_ws63_mac, &ref_record);
            if (ret != ESP_OK) {
                char error_buf[256];
                protocol_generate_error_response(ERR_ASSET_NOT_FOUND, "Reference asset not found", 
                                                error_buf, sizeof(error_buf));
                ws63_send_json_raw(error_buf);
                break;
            }
            
            // 匹配特征
            extern bool ai_module_match_features(const float *feature1, const float *feature2, 
                                                int size, asset_class_t asset_class, 
                                                similarity_result_t *result);
            
            ai_module_match_features(g_ws63_front_feature, ref_record.front_feature, 
                                    FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &front_result);
            ai_module_match_features(g_ws63_side_feature, ref_record.side_feature, 
                                    FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &side_result);
            ai_module_match_features(g_ws63_top_feature, ref_record.top_feature, 
                                    FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &top_result);
            
            // 计算加权置信度
            float weighted_conf = front_result.confidence * 0.5f +
                                 side_result.confidence * 0.3f +
                                 top_result.confidence * 0.2f;
            
            bool is_match = (weighted_conf >= front_result.match_threshold);
            
            // 生成响应
            cJSON *json_obj = cJSON_CreateObject();
            if (json_obj) {
                cJSON_AddStringToObject(json_obj, "type", "task_done");
                cJSON_AddStringToObject(json_obj, "task", "inventory");
                cJSON_AddStringToObject(json_obj, "mac", g_ws63_mac);
                cJSON_AddStringToObject(json_obj, "result", "success");
                cJSON_AddBoolToObject(json_obj, "is_match", is_match);
                cJSON_AddNumberToObject(json_obj, "weighted_confidence", weighted_conf);
                cJSON_AddNumberToObject(json_obj, "front_confidence", front_result.confidence);
                cJSON_AddNumberToObject(json_obj, "side_confidence", side_result.confidence);
                cJSON_AddNumberToObject(json_obj, "top_confidence", top_result.confidence);
                cJSON_AddNumberToObject(json_obj, "threshold", front_result.match_threshold);
                cJSON_AddStringToObject(json_obj, "item_name", g_ws63_item_name);
                cJSON_AddStringToObject(json_obj, "storage_area", (char[2]){g_ws63_storage_area, '\0'});
                cJSON_AddNumberToObject(json_obj, "quantity", g_ws63_quantity);
                
                char *json_str = cJSON_PrintUnformatted(json_obj);
                if (json_str) {
                    char response_buf[512];
                    strncpy(response_buf, json_str, sizeof(response_buf) - 1);
                    ws63_send_json_raw(response_buf);
                    free(json_str);
                }
                cJSON_Delete(json_obj);
            }
            break;
        }
            
        case CMD_OUTBOUND: {
            // 加载参考特征
            asset_record_t ref_record;
            ret = asset_load(g_ws63_mac, &ref_record);
            if (ret != ESP_OK) {
                char error_buf[256];
                protocol_generate_error_response(ERR_ASSET_NOT_FOUND, "Reference asset not found", 
                                                error_buf, sizeof(error_buf));
                ws63_send_json_raw(error_buf);
                break;
            }
            
            // 匹配特征
            similarity_result_t out_result = {0};
            extern bool ai_module_match_features(const float *feature1, const float *feature2, 
                                                int size, asset_class_t asset_class, 
                                                similarity_result_t *result);
            
            ai_module_match_features(g_ws63_front_feature, ref_record.front_feature, 
                                    FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &out_result);
            
            bool is_match = (out_result.confidence >= out_result.match_threshold);
            
            if (is_match) {
                // 更新数量
                asset_record_t update_record;
                memcpy(&update_record, &ref_record, sizeof(update_record));
                
                if (g_ws63_remove_qty >= update_record.quantity) {
                    update_record.quantity = 0;
                } else {
                    update_record.quantity -= g_ws63_remove_qty;
                }
                
                if (update_record.quantity == 0) {
                    ret = asset_delete(g_ws63_mac);
                } else {
                    bool is_overwrite = false;
                    ret = asset_save(&update_record, &is_overwrite);
                }
            }
            
            // 生成响应
            cJSON *json_obj = cJSON_CreateObject();
            if (json_obj) {
                cJSON_AddStringToObject(json_obj, "type", "task_done");
                cJSON_AddStringToObject(json_obj, "task", "outbound");
                cJSON_AddStringToObject(json_obj, "mac", g_ws63_mac);
                cJSON_AddStringToObject(json_obj, "result", "success");
                cJSON_AddBoolToObject(json_obj, "is_match", is_match);
                if (is_match) {
                    cJSON_AddNumberToObject(json_obj, "confidence", out_result.confidence);
                    cJSON_AddNumberToObject(json_obj, "threshold", out_result.match_threshold);
                    cJSON_AddStringToObject(json_obj, "item_name", g_ws63_item_name);
                    cJSON_AddNumberToObject(json_obj, "original_qty", g_ws63_original_qty);
                    cJSON_AddNumberToObject(json_obj, "remove_qty", g_ws63_remove_qty);
                    cJSON_AddNumberToObject(json_obj, "remaining_qty", 
                                           g_ws63_original_qty - g_ws63_remove_qty);
                    cJSON_AddBoolToObject(json_obj, "asset_deleted", 
                                         (g_ws63_original_qty - g_ws63_remove_qty) == 0);
                }
                
                char *json_str = cJSON_PrintUnformatted(json_obj);
                if (json_str) {
                    char response_buf[512];
                    strncpy(response_buf, json_str, sizeof(response_buf) - 1);
                    ws63_send_json_raw(response_buf);
                    free(json_str);
                }
                cJSON_Delete(json_obj);
            }
            break;
        }
            
        default:
            ESP_LOGE(TAG, "Unknown task type in finalize: %d", g_ws63_current_task);
            ret = ESP_ERR_INVALID_ARG;
            break;
    }
    
    // 重置状态
    ws63_reset_state();
    
    // 关闭摄像头
    camera_module_deinit();
    
    return ret;
}

/**
 * @brief 重置WS63状态
 */
static void ws63_reset_state(void)
{
    g_ws63_state = WS63_STATE_IDLE;
    g_ws63_current_task = CMD_UNKNOWN;
    memset(g_ws63_mac, 0, sizeof(g_ws63_mac));
    memset(g_ws63_item_name, 0, sizeof(g_ws63_item_name));
    g_ws63_storage_area = 'A';
    g_ws63_quantity = 0;
    g_ws63_remove_qty = 0;
    g_ws63_original_qty = 0;
    g_ws63_total_views = 0;
    g_ws63_captured_views = 0;
    memset(g_ws63_front_feature, 0, sizeof(g_ws63_front_feature));
    memset(g_ws63_side_feature, 0, sizeof(g_ws63_side_feature));
    memset(g_ws63_top_feature, 0, sizeof(g_ws63_top_feature));
    
    ESP_LOGI(TAG, "WS63 state reset to IDLE");
}

/**
 * @brief WS63 UART1 接收任务
 */
void ws63_recv_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WS63 UART receive task started");
    
    uint8_t *data = (uint8_t *)malloc(WS63_UART_BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        vTaskDelete(NULL);
        return;
    }
    
    char *line_buf = (char *)malloc(WS63_UART_BUF_SIZE);
    if (!line_buf) {
        ESP_LOGE(TAG, "Failed to allocate line buffer for WS63 receive task");
        free(data);
        vTaskDelete(NULL);
        return;
    }
    
    int line_pos = 0;
    
    while (1) {
        int len = uart_read_bytes(WS63_UART_NUM, data, WS63_UART_BUF_SIZE, 
                                  100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t ch = data[i];
                
                if (ch == '\n' || ch == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received from WS63: %s", line_buf);
                        protocol_handle_command(line_buf);
                        line_pos = 0;
                    }
                } else {
                    if (line_pos < WS63_UART_BUF_SIZE - 1) {
                        line_buf[line_pos++] = ch;
                    }
                }
            }
        }
        
        // 复位看门狗，防止超时重启
        esp_task_wdt_reset();
    }
    
    // 理论上不会到达这里
    free(line_buf);
    free(data);
    vTaskDelete(NULL);
}
