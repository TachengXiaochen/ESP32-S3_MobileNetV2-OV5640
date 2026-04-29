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

// 特征向量大小由 asset_manager.h 统一定义 (FEATURE_VEC_SIZE = 1280)
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
        "  r: 注册（入库）新资产\r\n"
        "  c: 进入盘点引导模式\r\n"
        "  o: 出库资产\r\n"
        "  d: 进入删除资产模式\r\n"
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
        "  r - Register new asset (入库)\r\n"
        "  o - Outbound asset (出库)\r\n"
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
        "  Step 1: Input MAC address to register:\r\n"
        "  Format: XX:XX:XX:XX:XX:XX\r\n"
        "  Example: AA:BB:CC:DD:EE:FF\r\n"
        "=========================================\r\n"
        "[GUIDE] Input MAC address: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册模式第二步引导（输入物品名称）
 */
static void show_registration_name_guide(const char *mac)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== REGISTRATION MODE ==========\r\n"
             "  MAC: %s\r\n"
             "  Step 2: Input item name:\r\n"
             "  Example: Wooden Chair, Steel Bolt M8\r\n"
             "=========================================\r\n"
             "[GUIDE] Input item name: ",
             mac);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册模式第三步引导（输入存放区域）
 */
static void show_registration_area_guide(void)
{
    const char *guide = 
        "\r\n========== REGISTRATION MODE ==========\r\n"
        "  Step 3: Input storage area (A-Z):\r\n"
        "  Example: A, B, C...\r\n"
        "=========================================\r\n"
        "[GUIDE] Input storage area: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册模式第四步引导（输入数量）
 */
static void show_registration_quantity_guide(void)
{
    const char *guide = 
        "\r\n========== REGISTRATION MODE ==========\r\n"
        "  Step 4: Input quantity (positive integer):\r\n"
        "  Example: 1, 10, 100...\r\n"
        "=========================================\r\n"
        "[GUIDE] Input quantity: ";
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
        extern bool g_is_outbound_mode;
        g_is_outbound_mode = false;
        
        // ✅ 清空推理队列并重置计数器
        extern QueueHandle_t xInferenceQueue;
        extern int g_views_enqueued;
        extern int g_views_processed;
        extern int g_total_views;
        inference_job_t discard_job;
        while (xQueueReceive(xInferenceQueue, &discard_job, 0) == pdTRUE) {}
        g_views_enqueued = 0;
        g_views_processed = 0;
        g_total_views = 0;
        
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
        
        // 4.3 出库模式选择
        if (strcasecmp(cmd_buf, "o") == 0) {
            ESP_LOGI(TAG, "Entering outbound mode");
            g_camera_state = CAM_STATE_WAITING_OUT_MAC;  // 等待出库MAC
            g_inventory_state = INVENTORY_IDLE;
            const char *guide = 
                "\r\n========== OUTBOUND MODE ==========\r\n"
                "  Please input MAC address to outbound:\r\n"
                "  Format: XX:XX:XX:XX:XX:XX\r\n"
                "  Example: AA:BB:CC:DD:EE:FF\r\n"
                "======================================\r\n"
                "[GUIDE] Input MAC address: ";
            uart_write_bytes(UART_NUM_0, guide, strlen(guide));
            return;
        }

        // 4.4 删除模式选择
        if (strcasecmp(cmd_buf, "d") == 0) {
            ESP_LOGI(TAG, "Entering delete mode");
            g_camera_state = CAM_STATE_WAITING_DEL_MAC;  // 等待删除MAC
            show_delete_mode_guide();
            return;
        }
        
        // 4.5 无效命令
        uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
        return;
    }
    
    // ========== 5. 等待注册MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_REG_MAC) {
        if (cmd_handler_validate_mac(cmd_buf)) {
            // 保存MAC地址到全局变量
            snprintf(g_current_mac, MAC_ADDR_LEN + 1, "%s", cmd_buf);
            
            // 进入物品名称输入阶段
            g_camera_state = CAM_STATE_WAITING_REG_NAME;
            show_registration_name_guide(cmd_buf);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid MAC address format.\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Expected format: XX:XX:XX:XX:XX:XX\r\n", 37);
            uart_write_bytes(UART_NUM_0, "Example: AA:BB:CC:DD:EE:FF\r\n", 29);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input MAC address: ", 27);
            return;
        }
    }
    
    // ========== 5b. 等待注册物品名称状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_REG_NAME) {
        if (strlen(cmd_buf) > 0 && strlen(cmd_buf) < 128) {
            extern char g_reg_item_name[];
            snprintf(g_reg_item_name, 128, "%s", cmd_buf);
            g_camera_state = CAM_STATE_WAITING_REG_AREA;
            show_registration_area_guide();
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Item name is required (1-127 chars).\r\n", 44);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input item name: ", 25);
            return;
        }
    }
    
    // ========== 5c. 等待注册存放区域状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_REG_AREA) {
        if (strlen(cmd_buf) == 1 && isalpha((unsigned char)cmd_buf[0])) {
            extern char g_reg_storage_area;
            g_reg_storage_area = toupper((unsigned char)cmd_buf[0]);
            g_camera_state = CAM_STATE_WAITING_REG_QUANTITY;
            show_registration_quantity_guide();
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a single letter (A-Z).\r\n", 44);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input storage area: ", 28);
            return;
        }
    }
    
    // ========== 5d. 等待注册数量状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_REG_QUANTITY) {
        // 验证是否为正整数
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) {
            if (!isdigit((unsigned char)cmd_buf[i])) {
                valid = false;
                break;
            }
        }
        
            if (valid && strlen(cmd_buf) > 0) {
            extern uint32_t g_reg_quantity;
            g_reg_quantity = (uint32_t)atoi(cmd_buf);
            if (g_reg_quantity == 0) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Quantity must be greater than 0.\r\n", 40);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity: ", 24);
                return;
            }
            
            // ✅ 信息收集完成，设置模式标志为注册模式，初始化硬件
            extern bool g_is_inventory_mode;
            extern bool g_is_outbound_mode;
            g_is_inventory_mode = false;
            g_is_outbound_mode = false;
            g_inventory_state = INVENTORY_IDLE;
            
            // ✅ 设置期望视图数：注册模式=3 (f+s+t)
            extern int g_total_views;
            g_total_views = 3;
            
            // LED指示：注册模式
            extern void led_camera_registration(void);
            led_camera_registration();
            
            // 显示注册信息摘要
            char summary[384];
            snprintf(summary, sizeof(summary),
                     "\r\n========== REGISTRATION SUMMARY ==========\r\n"
                     "  MAC:          %s\r\n"
                     "  Item Name:    %s\r\n"
                     "  Storage Area: %c\r\n"
                     "  Quantity:     %lu\r\n"
                     "===========================================\r\n"
                     "[SYSTEM] Initializing camera...\r\n",
                     g_current_mac, g_reg_item_name, g_reg_storage_area, (unsigned long)g_reg_quantity);
            uart_write_bytes(UART_NUM_0, summary, strlen(summary));
            
            // 初始化硬件并开始拍摄流程
            handle_mac_initialization(g_current_mac);
            
            // 显示注册第一步引导
            show_registration_step1(g_current_mac);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a valid positive integer.\r\n", 45);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity: ", 24);
            return;
        }
    }
    
    // ========== 5e. 等待出库MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_OUT_MAC) {
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
                // 资产存在，显示当前信息
                char info_msg[320];
                snprintf(info_msg, sizeof(info_msg),
                         "\r\n========== OUTBOUND MODE ==========\r\n"
                         "  MAC: %s\r\n"
                         "  Item: %s\r\n"
                         "  Area: %c\r\n"
                         "  Stock: %lu\r\n"
                         "===================================\r\n",
                         record->mac_address, record->item_name, record->storage_area,
                         (unsigned long)record->quantity);
                uart_write_bytes(UART_NUM_0, info_msg, strlen(info_msg));
                
                // 保存MAC地址
                snprintf(g_current_mac, MAC_ADDR_LEN + 1, "%s", cmd_buf);
                
                free(record);
                
                // 进入输入出库数量阶段
                g_camera_state = CAM_STATE_WAITING_OUT_QTY;
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to remove: ", 34);
                return;
            } else {
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
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input MAC address: ", 27);
            return;
        }
    }
    
    // ========== 5f. 等待出库数量状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_OUT_QTY) {
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) {
            if (!isdigit((unsigned char)cmd_buf[i])) {
                valid = false;
                break;
            }
        }
        
        if (valid && strlen(cmd_buf) > 0) {
            extern uint32_t g_outbound_quantity;
            g_outbound_quantity = (uint32_t)atoi(cmd_buf);
            if (g_outbound_quantity == 0) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Quantity must be greater than 0.\r\n", 40);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to remove: ", 34);
                return;
            }
            
            // 设置出库模式标志
            extern bool g_is_inventory_mode;
            extern bool g_is_outbound_mode;
            g_is_inventory_mode = false;
            g_is_outbound_mode = true;
            g_inventory_state = INVENTORY_IDLE;
            
            // ✅ 设置期望视图数：出库模式=1 (仅f)
            extern int g_total_views;
            g_total_views = 1;
            
            // LED指示
            extern void led_camera_inventory(void);
            led_camera_inventory();
            
            char summary[320];
            snprintf(summary, sizeof(summary),
                     "\r\n========== OUTBOUND ============\r\n"
                     "  MAC:      %s\r\n"
                     "  Remove:   %lu\r\n"
                     "  [STEP 1/1] Capture FRONT view\r\n"
                     "           -> Send 'f' to capture\r\n"
                     "====================================\r\n",
                     g_current_mac, (unsigned long)g_outbound_quantity);
            uart_write_bytes(UART_NUM_0, summary, strlen(summary));
            
            // 初始化硬件
            handle_mac_initialization(g_current_mac);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a valid positive integer.\r\n", 45);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to remove: ", 34);
            return;
        }
    }
    
    // ========== 6. 等待盘点MAC地址状态 ==========
    if (g_camera_state == CAM_STATE_WAITING_INV_MAC) {
        if (cmd_handler_validate_mac(cmd_buf)) {
            // ✅ 设置模式标志为盘点模式
            extern bool g_is_inventory_mode;
            g_is_inventory_mode = true;
            
            // ✅ 设置期望视图数：盘点模式=3 (f+s+t)
            extern int g_total_views;
            g_total_views = 3;
            
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
