#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "cmd_handler.h"
#include "asset_manager.h"
#include "main.h"  // 需要访问全局变量和队列

// 定义MIN宏（如果未定义）
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "cmd_handler";

// 外部引用（从 main.c）
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern char g_current_mac[];
extern camera_state_t g_camera_state;
extern view_state_t g_view_state;
extern inventory_state_t g_inventory_state;
extern float g_front_feature[];
extern float g_side_feature[];
extern float g_top_feature[];
extern bool g_camera_ready;
extern bool g_storage_ready;

// MAC地址长度常量
#ifndef MAC_ADDR_LEN
#define MAC_ADDR_LEN 17
#endif

// 特征向量大小常量 (如果未在头文件中定义)
// 注意：请根据实际模型输出维度调整此值 (例如 MobileNet v1 可能是 128, 512, 1024 等)
#ifndef FEATURE_VEC_SIZE
#define FEATURE_VEC_SIZE 128
#endif

/**
 * @brief 验证MAC地址格式
 */
bool cmd_handler_validate_mac(const char *mac)
{
    if (mac == NULL || strlen(mac) != MAC_ADDR_LEN) {
        return false;
    }
    
    // 格式: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < MAC_ADDR_LEN; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (mac[i] != ':') {
                return false;
            }
        } else {
            char c = mac[i];
            if (!((c >= '0' && c <= '9') || 
                  (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief 显示帮助信息
 */
void cmd_handler_show_help(void)
{
    const char *help_text = 
        "\r\n[HELP] Command List:\r\n"
        "  MAC地址: AA:BB:CC:DD:EE:FF\r\n"
        "  f/s/t: 拍摄正/侧/顶视图\r\n"
        "  c: 进入盘点引导模式\r\n"
        "  d: 进入删除资产模式\r\n"
        "  p XX:XX:XX:XX:XX:XX: 指定MAC盘点\r\n"
        "  l: 列出所有资产\r\n"
        "  i: 显示系统信息\r\n"
        "  exit/quit: 强制退出到主菜单（任何阶段）\r\n"
        "  help/?: 显示本帮助\r\n";
    
    uart_write_bytes(UART_NUM_0, help_text, strlen(help_text));
}

/**
 * @brief 显示主菜单（开机/任务完成后）
 */
void show_main_menu(void)
{
    const char *menu = 
        "\r\n========== MAIN MENU ==========\r\n"
        "  r - Register new asset\r\n"
        "  c - Inventory existing asset\r\n"
        "  d - Delete asset\r\n"
        "  l - List all assets\r\n"
        "  i - System information\r\n"
        "  help/? - Show this menu\r\n"
        "================================\r\n"
        "[GUIDE] Please select an option: ";
    uart_write_bytes(UART_NUM_0, menu, strlen(menu));
}

/**
 * @brief 显示注册模式引导（输入r后）
 */
static void show_registration_mode_guide(void)
{
    const char *guide = 
        "\r\n========== REGISTRATION MODE ==========\r\n"
        "  Please input MAC address to register:\r\n"
        "  Format: XX:XX:XX:XX:XX:XX\r\n"
        "  Example: AA:BB:CC:DD:EE:FF\r\n"
        "=========================================\r\n"
        "[GUIDE] Input MAC address: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示盘点模式引导（输入c后）
 */
static void show_inventory_mode_guide(void)
{
    const char *guide = 
        "\r\n========== INVENTORY MODE ==========\r\n"
        "  Please input MAC address to inventory:\r\n"
        "  Format: XX:XX:XX:XX:XX:XX\r\n"
        "  Example: AA:BB:CC:DD:EE:FF\r\n"
        "======================================\r\n"
        "[GUIDE] Input MAC address: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示删除模式引导（输入d后）
 */
static void show_delete_mode_guide(void)
{
    // ✅ 先显示当前资产列表(不带标题,因为asset_list_uart内部已有标题)
    extern void asset_list_uart(void);
    asset_list_uart();
    
    // 添加分隔线,使界面更清晰
    uart_write_bytes(UART_NUM_0, "\r\n", 2);
    
    // 再显示删除模式引导
    const char *guide = 
        "========== DELETE MODE ==========\r\n"
        "  Please input MAC address to delete:\r\n"
        "  Format: XX:XX:XX:XX:XX:XX\r\n"
        "  Example: AA:BB:CC:DD:EE:FF\r\n"
        "===================================\r\n"
        "[GUIDE] Input MAC address: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册流程第一步引导
 */
void show_registration_step1(const char *mac)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== REGISTRATION ==========\r\n"
             "  Target MAC: %s\r\n"
             "  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view\r\n"
             "           -> Send 'f' to capture\r\n"
             "====================================\r\n",
             mac);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册流程第二步引导
 */
void show_registration_step2(void)
{
    const char *guide = 
        "\r\n[STEP 2/3] Capture SIDE view\r\n"
        "         -> Send 's' to capture\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册流程第三步引导
 */
void show_registration_step3(void)
{
    const char *guide = 
        "\r\n[STEP 3/3] Capture TOP view\r\n"
        "         -> Send 't' to capture and save\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示盘点流程第一步引导
 */
void show_inventory_step1(const char *mac)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== INVENTORY ============\r\n"
             "  Target MAC: %s\r\n"
             "  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view\r\n"
             "           -> Send 'f' to capture\r\n"
             "====================================\r\n",
             mac);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示盘点流程第二步引导
 */
void show_inventory_step2(void)
{
    const char *guide = 
        "\r\n[STEP 2/3] Capture SIDE view\r\n"
        "         -> Send 's' to capture\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示盘点流程第三步引导
 */
void show_inventory_step3(void)
{
    const char *guide = 
        "\r\n[STEP 3/3] Capture TOP view\r\n"
        "         -> Send 't' to capture and analyze\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册完成提示
 */
static void show_registration_complete(const char *mac)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "\r\n✅ REGISTRATION COMPLETE!\r\n"
             "  Asset saved to SD card successfully.\r\n"
             "  MAC: %s\r\n"
             "  Camera: POWER OFF\r\n"
             "\r\n", mac);
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
    
    // 返回主菜单
    show_main_menu();
}

/**
 * @brief 显示盘点结果并返回主菜单
 */
static void show_inventory_result(float conf_front, float conf_side, float conf_top, 
                                  float weighted_conf, const char *mac)
{
    char result[512];
    snprintf(result, sizeof(result),
             "\r\n========== INVENTORY RESULT ==========\r\n"
             "  Front: %.2f (×0.5)\r\n"
             "  Side:  %.2f (×0.3)\r\n"
             "  Top:   %.2f (×0.2)\r\n"
             "  ----------------------------------------\r\n"
             "  Weighted Confidence: %.4f\r\n"
             "  MAC: %s\r\n"
             "  Camera: POWER OFF\r\n"
             "========================================\r\n",
             conf_front, conf_side, conf_top, weighted_conf, mac);
    uart_write_bytes(UART_NUM_0, result, strlen(result));
    
    // 返回主菜单
    show_main_menu();
}

/**
 * @brief 显示存储未初始化错误
 */
static void show_storage_not_ready(void)
{
    const char *msg = 
        "\r\n⚠️  ERROR: Storage not initialized!\r\n"
        "  Please wait for system initialization...\r\n"
        "  Or check SD card connection.\r\n\r\n";
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
    
    show_main_menu();
}

/**
 * @brief 处理存储相关命令
 */
static void handle_storage_command(const char *cmd)
{
    if (strcasecmp(cmd, "storage sd") == 0) {
        uart_write_bytes(UART_NUM_0, "Switching to SD Card mode...\r\n", 33);
        esp_err_t ret = asset_switch_storage_mode(STORAGE_MODE_SD_CARD);
        if (ret == ESP_OK) {
            uart_write_bytes(UART_NUM_0, "Storage switched to SD Card\r\n", 32);
        } else {
            uart_write_bytes(UART_NUM_0, "Failed to switch to SD Card mode\r\n", 36);
        }
    } else if (strcasecmp(cmd, "storage flash") == 0) {
        uart_write_bytes(UART_NUM_0, "Switching to SPIFFS (Flash) mode...\r\n", 40);
        esp_err_t ret = asset_switch_storage_mode(STORAGE_MODE_SPIFFS);
        if (ret == ESP_OK) {
            uart_write_bytes(UART_NUM_0, "Storage switched to SPIFFS (Internal Flash)\r\n", 47);
        } else {
            uart_write_bytes(UART_NUM_0, "Failed to switch to SPIFFS mode\r\n", 35);
        }
    } else if (strcasecmp(cmd, "storage status") == 0) {
        storage_mode_t mode = asset_get_storage_mode();
        char msg[128];
        snprintf(msg, sizeof(msg), "Current storage mode: %s\r\n", 
                 mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS (Internal Flash)");
        uart_write_bytes(UART_NUM_0, msg, strlen(msg));
    }
}

/**
 * @brief 处理信息查询命令
 */
static void handle_info_command(const char *cmd)
{
    if (strcasecmp(cmd, "l") == 0 || strcasecmp(cmd, "list") == 0) {
        extern void asset_list_uart(void);
        asset_list_uart();
    } else if (strcasecmp(cmd, "i") == 0 || strcasecmp(cmd, "info") == 0) {
        extern void print_system_info_uart(void);
        print_system_info_uart();
    }
}

/**
 * @brief 处理MAC地址初始化命令 (通用)
 */
static void handle_mac_initialization(const char *mac)
{
    // ✅ 修复：使用MAC_ADDR_LEN + 1代替sizeof，避免不完整类型问题
    snprintf(g_current_mac, MAC_ADDR_LEN + 1, "%s", mac);
    
    ESP_LOGI(TAG, "MAC address received: %s", mac);
    uart_write_bytes(UART_NUM_0, "\r\n[SYSTEM] Initializing hardware...\r\n", 37);
    
    // 0. 初始化AI模块（加载MobileNet模型）
    extern bool ai_module_init(void);
    if (!ai_module_init()) {
        uart_write_bytes(UART_NUM_0, "[ERROR] AI module initialization failed!\r\n", 42);
        return;
    }
    
    // 1. 发送初始化存储指令
    system_msg_t init_storage_msg = {0};
    init_storage_msg.cmd = CMD_INIT_STORAGE;
    xQueueSend(xStorageQueue, &init_storage_msg, portMAX_DELAY);
    
    // 2. 发送初始化摄像头指令
    system_msg_t init_camera_msg = {0};
    init_camera_msg.cmd = CMD_INIT_CAMERA;
    snprintf(init_camera_msg.mac, sizeof(init_camera_msg.mac), "%s", mac);
    xQueueSend(xSystemQueue, &init_camera_msg, portMAX_DELAY);
    
    // 更新状态
    g_camera_state = CAM_STATE_READY;
    g_view_state = VIEW_NONE;
    
    // 3. 根据当前模式显示对应的操作引导
    if (g_inventory_state == INVENTORY_IDLE) {
        // 注册模式：显示第一步引导
        show_registration_step1(mac);
    } else {
        // 盘点模式：显示盘点第一步引导
        show_inventory_step1(mac);
    }
}

/**
 * @brief 处理单条命令（完全重构版 - 支持状态机）
 */
void cmd_handler_process(const char *cmd_line)
{
    if (cmd_line == NULL || strlen(cmd_line) == 0) {
        return;
    }
    
    // 去除首尾空白字符
    const char *cmd = cmd_line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\t')) {
        len--;
    }
    
    // 创建临时副本用于处理
    char cmd_buf[128];
    strncpy(cmd_buf, cmd, MIN(len, sizeof(cmd_buf) - 1));
    cmd_buf[MIN(len, sizeof(cmd_buf) - 1)] = '\0';
    
    // ========== 0. 强制退出命令（任何状态都可用）==========
    if (strcasecmp(cmd_buf, "exit") == 0 || strcasecmp(cmd_buf, "quit") == 0) {
        uart_write_bytes(UART_NUM_0, "\r\n[EXIT] Returning to main menu...\r\n", 38);
        
        // 重置所有状态
        g_camera_state = CAM_STATE_WAITING_MAC;
        g_view_state = VIEW_NONE;
        g_inventory_state = INVENTORY_IDLE;
        extern bool g_is_inventory_mode;
        g_is_inventory_mode = false;  // ✅ 重置模式标志
        
        // 关闭摄像头
        extern bool g_camera_power_on;
        extern void led_camera_off(void);
        extern esp_err_t camera_module_deinit(void);
        
        if (g_camera_power_on) {
            led_camera_off();
            g_camera_power_on = false;
            camera_module_deinit();
            uart_write_bytes(UART_NUM_0, "Camera: POWER OFF\r\n", 21);
        }
        
        // 显示主菜单
        show_main_menu();
        return;
    }
    
    // ========== 1. 存储管理命令（任何状态都可用）==========
    if (strncasecmp(cmd_buf, "storage ", 8) == 0) {
        handle_storage_command(cmd_buf);
        return;
    }
    
    // ========== 2. 信息查询命令（任何状态都可用）==========
    if (strcasecmp(cmd_buf, "l") == 0 || strcasecmp(cmd_buf, "list") == 0) {
        // ✅ 如果存储未初始化，尝试重新初始化
        if (!g_storage_ready) {
            ESP_LOGW(TAG, "Storage not ready, attempting to initialize...");
            extern bool storage_module_init(void);
            if (storage_module_init()) {
                g_storage_ready = true;
                ESP_LOGI(TAG, "Storage initialized successfully on demand");
            } else {
                uart_write_bytes(UART_NUM_0, 
                    "\r\n⚠️  ERROR: Storage initialization failed!\r\n"
                    "Please check SD card connection.\r\n\r\n", 72);
                show_main_menu();
                return;
            }
        }
        handle_info_command(cmd_buf);
        return;
    }
    
    if (strcasecmp(cmd_buf, "i") == 0 || strcasecmp(cmd_buf, "info") == 0) {
        handle_info_command(cmd_buf);
        return;
    }
    
    // ========== 3. 帮助命令（任何状态都可用）==========
    if (strcasecmp(cmd_buf, "help") == 0 || strcmp(cmd_buf, "?") == 0) {
        cmd_handler_show_help();
        return;
    }
    
    // ========== 4. 主菜单状态下的模式选择 ==========
    if (g_camera_state == CAM_STATE_WAITING_MAC) {
        // 4.1 注册模式选择
        if (strcasecmp(cmd_buf, "r") == 0) {
            ESP_LOGI(TAG, "Entering registration mode");
            g_camera_state = CAM_STATE_WAITING_REG_MAC;  // 等待注册MAC
            g_inventory_state = INVENTORY_IDLE;  // 重置盘点状态
            show_registration_mode_guide();
            return;
        }
        
        // 4.2 盘点模式选择
        if (strcasecmp(cmd_buf, "c") == 0) {
            ESP_LOGI(TAG, "Entering inventory mode");
            g_camera_state = CAM_STATE_WAITING_INV_MAC;  // 等待盘点MAC
            show_inventory_mode_guide();
            return;
        }
        
        // 4.3 删除模式选择
        if (strcasecmp(cmd_buf, "d") == 0) {
            ESP_LOGI(TAG, "Entering delete mode");
            g_camera_state = CAM_STATE_WAITING_DEL_MAC;  // 等待删除MAC
            show_delete_mode_guide();
            return;
        }
        
        // 4.4 无效命令
        uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
        return;
    }
    
    // ========== 5. 等待注册MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_REG_MAC) {
        if (cmd_handler_validate_mac(cmd_buf)) {
            // ✅ 设置模式标志为注册模式
            extern bool g_is_inventory_mode;
            g_is_inventory_mode = false;
            
            // LED指示：注册模式 - 绿色常亮
            extern void led_camera_registration(void);
            led_camera_registration();
            
            handle_mac_initialization(cmd_buf);
            
            // 重置盘点状态为空闲，准备开始注册流程
            g_inventory_state = INVENTORY_IDLE; 
            
            // 显示注册第一步引导
            show_registration_step1(cmd_buf);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid MAC address format.\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Expected format: XX:XX:XX:XX:XX:XX\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Example: AA:BB:CC:DD:EE:FF\r\n", 29);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input MAC address: ", 27);
            return;
        }
    }
    
    // ========== 6. 等待盘点MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_INV_MAC) {
        if (cmd_handler_validate_mac(cmd_buf)) {
            // ✅ 设置模式标志为盘点模式
            extern bool g_is_inventory_mode;
            g_is_inventory_mode = true;
            
            // 检查该MAC是否已注册
            asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
            if (!record) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Memory allocation failed\r\n", 33);
                g_camera_state = CAM_STATE_WAITING_MAC;
                show_main_menu();
                return;
            }
            
            esp_err_t ret = asset_load(cmd_buf, record);
            if (ret == ESP_OK) {
                // ✅ 资产存在，加载到全局变量供后续对比使用
                extern float g_front_feature[];
                extern float g_side_feature[];
                extern float g_top_feature[];
                
                // ✅ 修复：使用宏定义的大小代替sizeof()，避免extern数组不完整类型错误
                memcpy(g_front_feature, record->front_feature, FEATURE_VEC_SIZE * sizeof(float));
                memcpy(g_side_feature, record->side_feature, FEATURE_VEC_SIZE * sizeof(float));
                memcpy(g_top_feature, record->top_feature, FEATURE_VEC_SIZE * sizeof(float));
                
                free(record);  // ✅ 释放临时内存
                
                // LED指示：盘点模式 - 蓝色常亮
                extern void led_camera_inventory(void);
                led_camera_inventory();
                
                ESP_LOGI(TAG, "Starting inventory for MAC: %s", cmd_buf);
                
                // 初始化硬件
                handle_mac_initialization(cmd_buf);
                
                // ✅ 设置盘点状态为等待正面拍摄（不发送分析命令）
                g_inventory_state = INVENTORY_WAITING_FRONT;
                
                // ✅ 显示盘点第一步引导，等待用户输入 f/s/t
                show_inventory_step1(cmd_buf);
                return;
            } else {
                // 资产不存在
                free(record);
                char err_msg[128];
                snprintf(err_msg, sizeof(err_msg), 
                         "\r\n[ERROR] Asset not found: %s\r\n", cmd_buf);
                uart_write_bytes(UART_NUM_0, err_msg, strlen(err_msg));
                uart_write_bytes(UART_NUM_0, "Please register this asset first.\r\n", 36);
                g_camera_state = CAM_STATE_WAITING_MAC;
                show_main_menu();
                return;
            }
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid MAC address format.\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Expected format: XX:XX:XX:XX:XX:XX\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Example: AA:BB:CC:DD:EE:FF\r\n", 29);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input MAC address: ", 27);
            return;
        }
    }
    
    // ========== 7. 等待删除MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_DEL_MAC) {
        if (cmd_handler_validate_mac(cmd_buf)) {
            // 检查该MAC是否已注册
            asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
            if (!record) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Memory allocation failed\r\n", 33);
                g_camera_state = CAM_STATE_WAITING_MAC;
                show_main_menu();
                return;
            }
            
            esp_err_t ret = asset_load(cmd_buf, record);
            if (ret == ESP_OK) {
                // 资产存在，确认删除
                char confirm_msg[256];
                snprintf(confirm_msg, sizeof(confirm_msg),
                         "\r\n⚠️  CONFIRM DELETE ASSET?\r\n"
                         "  MAC: %s\r\n"
                         "  Press 'y' to confirm, any other key to cancel: ",
                         cmd_buf);
                uart_write_bytes(UART_NUM_0, confirm_msg, strlen(confirm_msg));
                
                // 保存MAC地址用于后续确认
                snprintf(g_current_mac, MAC_ADDR_LEN + 1, "%s", cmd_buf);
                
                // 设置状态为等待确认
                g_camera_state = CAM_STATE_WAITING_DEL_CONFIRM;
                
                free(record);
                return;
            } else {
                // 资产不存在
                uart_write_bytes(UART_NUM_0, "\r\n❌ ASSET NOT FOUND\r\n", 24);
                uart_write_bytes(UART_NUM_0, "Asset with MAC ", 15);
                uart_write_bytes(UART_NUM_0, cmd_buf, strlen(cmd_buf));
                uart_write_bytes(UART_NUM_0, " does not exist.\r\n\r\n", 21);
                
                // 重置状态并返回主菜单
                g_camera_state = CAM_STATE_WAITING_MAC;
                show_main_menu();
                free(record);
                return;
            }
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid MAC address format.\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Expected format: XX:XX:XX:XX:XX:XX\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Example: AA:BB:CC:DD:EE:FF\r\n", 29);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input MAC address: ", 27);
            return;
        }
    }
    
    // ========== 8. 等待删除确认状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_DEL_CONFIRM) {
        if (strcasecmp(cmd_buf, "y") == 0) {
            // 用户确认删除，执行删除操作
            extern esp_err_t asset_delete(const char *mac);
            esp_err_t ret = asset_delete(g_current_mac);
            if (ret == ESP_OK) {
                uart_write_bytes(UART_NUM_0, "\r\n✅ ASSET DELETED SUCCESSFULLY!\r\n", 35);
                uart_write_bytes(UART_NUM_0, "Asset with MAC ", 15);
                uart_write_bytes(UART_NUM_0, g_current_mac, strlen(g_current_mac));
                uart_write_bytes(UART_NUM_0, " has been removed.\r\n\r\n", 24);
                
                // ✅ 删除成功后显示更新后的资产列表
                extern void asset_list_uart(void);
                asset_list_uart();
            } else {
                uart_write_bytes(UART_NUM_0, "\r\n❌ FAILED TO DELETE ASSET\r\n", 27);
                uart_write_bytes(UART_NUM_0, "An error occurred during deletion.\r\n\r\n", 39);
            }
        } else {
            // 用户取消删除
            uart_write_bytes(UART_NUM_0, "\r\n❌ DELETION CANCELLED\r\n", 25);
            uart_write_bytes(UART_NUM_0, "Asset was not deleted.\r\n\r\n", 28);
        }
        
        // 重置状态并返回主菜单
        g_camera_state = CAM_STATE_WAITING_MAC;
        show_main_menu();
        return;
    }
    
    // ========== 8. READY状态下的命令处理 ==========
    if (g_camera_state == CAM_STATE_READY) {
        // 7.1 拍摄命令（f/s/t）
        if (strlen(cmd_buf) == 1) {
            char view_cmd = tolower(cmd_buf[0]);
            
            system_msg_t msg = {0};
            // ✅ 修复：使用snprintf确保MAC地址字符串正确终止
            snprintf(msg.mac, sizeof(msg.mac), "%s", g_current_mac);
            
            if (view_cmd == 'f') {
                msg.cmd = CMD_CAPTURE_FRONT;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                
                // 根据当前盘点状态显示下一步引导
                if (g_inventory_state == INVENTORY_WAITING_FRONT || g_inventory_state == INVENTORY_IDLE) {
                     // 如果是注册流程 (IDLE) 或 盘点第一步完成后
                     // 注意：这里假设发送命令后状态会在别处更新，或者我们仅仅提示下一步
                     // 通常拍摄命令发出后，等待图像捕获完成回调再更新状态更稳妥
                     // 但为了交互体验，这里可以提示下一步
                     if (g_inventory_state == INVENTORY_IDLE) {
                         show_registration_step2();
                     } else {
                         show_inventory_step2();
                     }
                }
            } else if (view_cmd == 's') {
                msg.cmd = CMD_CAPTURE_SIDE;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                
                // 根据当前盘点状态显示下一步引导
                if (g_inventory_state == INVENTORY_IDLE) {
                    show_registration_step3();
                } else {
                    show_inventory_step3();
                }
            } else if (view_cmd == 't') {
                msg.cmd = CMD_CAPTURE_TOP;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                // 不显示引导，等待任务完成后的提示 (show_registration_complete 或 show_inventory_result)
            } else {
                uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
            }
            return;
        }
    }
    
    // ========== 9. 未知命令 ==========
    uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
}

/**
 * @brief 初始化命令处理器
 */
void cmd_handler_init(void)
{
    ESP_LOGI(TAG, "Command handler initialized");
}
