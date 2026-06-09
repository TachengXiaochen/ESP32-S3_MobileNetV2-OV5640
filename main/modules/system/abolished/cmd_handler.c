#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "cmd_handler.h"
#include "asset_manager.h"
#include "tag_id_validator.h"
#include "verify_handler.h"
#include "main.h"  // 需要访问全局变量和队列

// 定义MIN宏（如果未定义）
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "cmd_handler";

// 外部变量声明
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern char g_current_tag_id[];
extern camera_state_t g_camera_state;
extern view_state_t g_view_state;
extern inventory_state_t g_inventory_state;
extern float g_front_feature[];
extern float g_side_feature[];
extern float g_top_feature[];
extern bool g_camera_ready;
extern bool g_storage_ready;

// MAC地址长度（如果未定义）
#ifndef MAC_ADDR_LEN
#define MAC_ADDR_LEN 17
#endif

/**
 * @brief 验证Tag ID是否有效
 */
bool cmd_handler_validate_tag_id(const char *tag_id)
{
    return tag_id_validator_validate(tag_id);
}

/**
 * @brief 显示验证现有资产的引导信息
 */
void show_verification_existing_guide(const char *tag_id, const char *item_name, 
                                       char storage_area, uint32_t current_qty)
{
    char guide[512];
    snprintf(guide, sizeof(guide),
             "\r\n========== VERIFICATION MODE ==========\r\n"
             "  Tag ID:      %s\r\n"
             "  Item:        %s\r\n"
             "  Area:        %c\r\n"
             "  Current Qty: %lu\r\n"
             "  [INFO] This Tag ID already exists.\r\n"
             "  Capture FRONT view to verify identity.\r\n"
             "  -> Send 'f' to capture and verify...\r\n"
             "==========================================\r\n",
             tag_id, item_name, storage_area, (unsigned long)current_qty);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示验证添加数量的引导信息
 */
void show_verification_add_qty_guide(const char *tag_id, const char *item_name, uint32_t current_qty)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\nâ?VERIFICATION PASSED!\r\n"
             "  Tag ID: %s\r\n"
             "  Item:   %s\r\n"
             "  Stock:  %lu\r\n"
             "\r\n[GUIDE] Input quantity to ADD (or 'q' to cancel): ",
             tag_id, item_name, (unsigned long)current_qty);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示验证失败的信息
 */
void show_verification_failed(float confidence, float threshold)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "\r\nâ?VERIFICATION FAILED!\r\n"
             "  Similarity: %.1f%% (threshold: %.0f%%)\r\n"
             "  Item mismatch! Quantity NOT updated.\r\n"
             "  Suggestion: Please check if correct item is placed.\r\n"
             "\r\n",
             confidence * 100.0f, threshold * 100.0f);
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
}

/**
 * @brief 显示验证重试的引导信息
 */
void show_verification_retry_guide(void)
{
    const char *guide = 
        "\r\n[GUIDE] Send 'f' to retry capture and verify,\r\n"
        "        or 'q' to cancel and return to main menu: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示帮助信息
 */
void cmd_handler_show_help(void)
{
    const char *help_text = 
        "\r\n[HELP] Command List:\r\n"
        "  Tag ID (新注册?:   0x0001-0xFFFF\r\n"
        "  MAC地址(新注册?:   AA:BB:CC:DD:EE:FF\r\n"
        "  f/s/t: 拍摄前/侧/顶视图\r\n"
        "  r: 注册新资产\r\n"
        "  c: 库存现有资产\r\n"
        "  o: 出库资产\r\n"
        "  d: 删除资产\r\n"
        "  l: 列出所有资产\r\n"
        "  i: 系统信息\r\n"
        "  exit/quit: 退出程序\r\n"
        "  help/?: 显示帮助信息\r\n";
    
    uart_write_bytes(UART_NUM_0, help_text, strlen(help_text));
}

/**
 * @brief 显示主菜单
 */
void show_main_menu(void)
{
    const char *menu = 
        "\r\n========== MAIN MENU ==========\r\n"
        "  r - Register new asset (注册)\r\n"
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
 * @brief 显示注册模式的引导信息
 */
static void show_registration_mode_guide(void)
{
    const char *guide = 
        "\r\n========== REGISTRATION MODE ==========\r\n"
        "  Step 1: Input Tag ID to register:\r\n"
        "  Format: 0x0001-0xFFFF\r\n"
        "  Example: 0x0001, 0x00AB, 0x1234\r\n"
        "=========================================\r\n"
        "[GUIDE] Input Tag ID: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册模式的名称引导信息
 */
static void show_registration_name_guide(const char *tag_id)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== REGISTRATION MODE ==========\r\n"
             "  Tag ID: %s\r\n"
             "  Step 2: Input item name:\r\n"
             "  Example: Wooden Chair, Steel Bolt M8\r\n"
             "=========================================\r\n"
             "[GUIDE] Input item name: ",
             tag_id);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册模式的区域引导信息
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
 * @brief 显示注册模式的数量引导信息
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
 * @brief 显示库存模式的引导信息
 */
static void show_inventory_mode_guide(void)
{
    const char *guide = 
        "\r\n========== INVENTORY MODE ==========\r\n"
        "  Please input Tag ID to inventory:\r\n"
        "  Format: 0x0001-0xFFFF\r\n"
        "  Example: 0x0001, 0x00AB\r\n"
        "======================================\r\n"
        "[GUIDE] Input Tag ID: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示删除模式的引导信息
 */
static void show_delete_mode_guide(void)
{
    // 先列出所有资产    extern void asset_list_uart(void);
    asset_list_uart();
    
    uart_write_bytes(UART_NUM_0, "\r\n", 2);
    
    const char *guide = 
        "========== DELETE MODE ==========\r\n"
        "  Please input Tag ID to delete:\r\n"
        "  Format: 0x0001-0xFFFF\r\n"
        "  Example: 0x0001, 0x00AB\r\n"
        "===================================\r\n"
        "[GUIDE] Input Tag ID: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册步骤1
 */
void show_registration_step1(const char *tag_id)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== REGISTRATION ==========\r\n"
             "  Target: %s\r\n"
             "  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view\r\n"
             "           -> Send 'f' to capture\r\n"
             "====================================\r\n",
             tag_id);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册步骤2
 */
void show_registration_step2(void)
{
    const char *guide = 
        "\r\n[STEP 2/3] Capture SIDE view\r\n"
        "         -> Send 's' to capture\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册步骤3
 */
void show_registration_step3(void)
{
    const char *guide = 
        "\r\n[STEP 3/3] Capture TOP view\r\n"
        "         -> Send 't' to capture and save\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示库存步骤1
 */
void show_inventory_step1(const char *tag_id)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== INVENTORY ============\r\n"
             "  Target: %s\r\n"
             "  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view\r\n"
             "           -> Send 'f' to capture\r\n"
             "====================================\r\n",
             tag_id);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示库存步骤2
 */
void show_inventory_step2(void)
{
    const char *guide = 
        "\r\n[STEP 2/3] Capture SIDE view\r\n"
        "         -> Send 's' to capture\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示库存步骤3
 */
void show_inventory_step3(void)
{
    const char *guide = 
        "\r\n[STEP 3/3] Capture TOP view\r\n"
        "         -> Send 't' to capture and analyze\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

/**
 * @brief 显示注册完成信息
 */
__attribute__((unused))
static void show_registration_complete(const char *tag_id)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "\r\nâ?REGISTRATION COMPLETE!\r\n"
             "  Asset saved to SD card successfully.\r\n"
             "  Tag ID: %s\r\n"
             "  Camera: POWER OFF\r\n"
             "\r\n", tag_id);
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
    
    show_main_menu();
}

/**
 * @brief 显示库存结果
 */
__attribute__((unused))
static void show_inventory_result(float conf_front, float conf_side, float conf_top, 
                                  float weighted_conf, const char *tag_id)
{
    char result[512];
    snprintf(result, sizeof(result),
             "\r\n========== INVENTORY RESULT ==========\r\n"
             "  Front: %.2f (Ă0.5)\r\n"
             "  Side:  %.2f (Ă0.3)\r\n"
             "  Top:   %.2f (Ă0.2)\r\n"
             "  ----------------------------------------\r\n"
             "  Weighted Confidence: %.4f\r\n"
             "  Tag ID: %s\r\n"
             "  Camera: POWER OFF\r\n"
             "========================================\r\n",
             conf_front, conf_side, conf_top, weighted_conf, tag_id);
    uart_write_bytes(UART_NUM_0, result, strlen(result));
    
    show_main_menu();
}

/**
 * @brief 显示存储未准备好信息
 */
__attribute__((unused))
static void show_storage_not_ready(void)
{
    const char *msg = 
        "\r\nâ ď¸  ERROR: Storage not initialized!\r\n"
        "  Please wait for system initialization...\r\n"
        "  Or check SD card connection.\r\n\r\n";
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
    
    show_main_menu();
}

/**
 * @brief 处理存储命令
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
 * @brief 处理信息命令
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
 * @brief 处理Tag ID初始化
 */
static void handle_tag_id_initialization(const char *tag_id)
{
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", tag_id);
    
    ESP_LOGI(TAG, "Tag ID received: %s", tag_id);
    uart_write_bytes(UART_NUM_0, "\r\n[SYSTEM] Initializing hardware...\r\n", 37);
    
    // 0. 初始化AI模块
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
    snprintf(init_camera_msg.tag_id, sizeof(init_camera_msg.tag_id), "%s", tag_id);
    xQueueSend(xSystemQueue, &init_camera_msg, portMAX_DELAY);
    
    // 更新状态
    g_camera_state = CAM_STATE_READY;
    g_view_state = VIEW_NONE;
    
    // 3. 根据当前模式显示对应的操作引导
    if (g_inventory_state == INVENTORY_IDLE) {
        show_registration_step1(tag_id);
    } else {
        show_inventory_step1(tag_id);
    }
}

/**
 * @brief 处理验证更新
 */
static void handle_verification_update(const char *tag_id, asset_record_t *existing)
{
    // 保存当前Tag ID
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", tag_id);
    
    // 显示验证现有资产信息
    show_verification_existing_guide(tag_id, existing->item_name, 
                                      existing->storage_area, existing->quantity);
    
    // 设置相机状态为验证现有资产
    g_camera_state = CAM_STATE_VERIFYING_EXISTING;
    
    // 初始化验证上下文
    verify_context_t verify_ctx;
    verify_handler_start(tag_id, existing, VERIFY_MODE_FAST, &verify_ctx);
    
    // 初始化AI模块
    extern bool ai_module_init(void);
    if (!ai_module_init()) {
        uart_write_bytes(UART_NUM_0, "[ERROR] AI module initialization failed!\r\n", 42);
        g_camera_state = CAM_STATE_WAITING_TAG_ID;
        show_main_menu();
        return;
    }
    
    system_msg_t init_storage_msg = {0};
    init_storage_msg.cmd = CMD_INIT_STORAGE;
    xQueueSend(xStorageQueue, &init_storage_msg, portMAX_DELAY);
    
    system_msg_t init_camera_msg = {0};
    init_camera_msg.cmd = CMD_INIT_CAMERA;
    snprintf(init_camera_msg.tag_id, sizeof(init_camera_msg.tag_id), "%s", tag_id);
    xQueueSend(xSystemQueue, &init_camera_msg, portMAX_DELAY);
}

/**
 * @brief 处理命令行
 */
void cmd_handler_process(const char *cmd_line)
{
    if (cmd_line == NULL || strlen(cmd_line) == 0) {
        return;
    }
    
    // 去除前导空格和制表符
    const char *cmd = cmd_line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\t')) {
        len--;
    }
    
    // 复制命令到缓冲区以便处理
    char cmd_buf[128];
    strncpy(cmd_buf, cmd, MIN(len, sizeof(cmd_buf) - 1));
    cmd_buf[MIN(len, sizeof(cmd_buf) - 1)] = '\0';
    
    // ========== 0. 退出命令处理 ==========
    if (strcasecmp(cmd_buf, "exit") == 0 || strcasecmp(cmd_buf, "quit") == 0) {
        uart_write_bytes(UART_NUM_0, "\r\n[EXIT] Returning to main menu...\r\n", 38);
        
        g_camera_state = CAM_STATE_WAITING_TAG_ID;
        g_view_state = VIEW_NONE;
        g_inventory_state = INVENTORY_IDLE;
        extern bool g_is_inventory_mode;
        g_is_inventory_mode = false;
        extern bool g_is_outbound_mode;
        g_is_outbound_mode = false;
        
        extern QueueHandle_t xInferenceQueue;
        extern int g_views_enqueued;
        extern int g_views_processed;
        extern int g_total_views;
        inference_job_t discard_job;
        while (xQueueReceive(xInferenceQueue, &discard_job, 0) == pdTRUE) {}
        g_views_enqueued = 0;
        g_views_processed = 0;
        g_total_views = 0;
        
        extern bool g_camera_power_on;
        extern void led_camera_off(void);
        extern esp_err_t camera_module_deinit(void);
        
        if (g_camera_power_on) {
            led_camera_off();
            g_camera_power_on = false;
            camera_module_deinit();
            uart_write_bytes(UART_NUM_0, "Camera: POWER OFF\r\n", 21);
        }
        
        show_main_menu();
        return;
    }
    
    // ========== 1. 存储命令处理 ==========
    if (strncasecmp(cmd_buf, "storage ", 8) == 0) {
        handle_storage_command(cmd_buf);
        return;
    }
    
    // ========== 2. 信息命令处理 ==========
    if (strcasecmp(cmd_buf, "l") == 0 || strcasecmp(cmd_buf, "list") == 0) {
        if (!g_storage_ready) {
            ESP_LOGW(TAG, "Storage not ready, attempting to initialize...");
            extern bool storage_module_init(void);
            if (storage_module_init()) {
                g_storage_ready = true;
                ESP_LOGI(TAG, "Storage initialized successfully on demand");
            } else {
                uart_write_bytes(UART_NUM_0, 
                    "\r\nâ ď¸  ERROR: Storage initialization failed!\r\n"
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
    
    // ========== 3. 帮助命令处理 ==========
    if (strcasecmp(cmd_buf, "help") == 0 || strcmp(cmd_buf, "?") == 0) {
        cmd_handler_show_help();
        return;
    }
    
    // ========== 4. 主菜单下的命令选择 ==========
    if (g_camera_state == CAM_STATE_WAITING_TAG_ID) {
        // 4.1 注册命令选择
        if (strcasecmp(cmd_buf, "r") == 0) {
            ESP_LOGI(TAG, "Entering registration mode");
            g_camera_state = CAM_STATE_WAITING_REG_TAG_ID;
            g_inventory_state = INVENTORY_IDLE;
            show_registration_mode_guide();
            return;
        }
        
        // 4.2 库存命令选择
        if (strcasecmp(cmd_buf, "c") == 0) {
            ESP_LOGI(TAG, "Entering inventory mode");
            g_camera_state = CAM_STATE_WAITING_INV_TAG_ID;
            show_inventory_mode_guide();
            return;
        }
        
        // 4.3 出库命令选择
        if (strcasecmp(cmd_buf, "o") == 0) {
            ESP_LOGI(TAG, "Entering outbound mode");
            g_camera_state = CAM_STATE_WAITING_OUT_TAG_ID;
            g_inventory_state = INVENTORY_IDLE;
            const char *guide = 
                "\r\n========== OUTBOUND MODE ==========\r\n"
                "  Please input Tag ID to outbound:\r\n"
                "  Format: 0x0001-0xFFFF\r\n"
                "  Example: 0x0001, 0x00AB\r\n"
                "======================================\r\n"
                "[GUIDE] Input Tag ID: ";
            uart_write_bytes(UART_NUM_0, guide, strlen(guide));
            return;
        }

        // 4.4 删除命令选择
        if (strcasecmp(cmd_buf, "d") == 0) {
            ESP_LOGI(TAG, "Entering delete mode");
            g_camera_state = CAM_STATE_WAITING_DEL_TAG_ID;
            show_delete_mode_guide();
            return;
        }
        
        // 4.5 未知命令
        uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
        return;
    }
    
    // ========== 5. 等待注册Tag ID ===========
    if (g_camera_state == CAM_STATE_WAITING_REG_TAG_ID) {
        // 验证是否为有效的Tag ID
        bool is_tag_id = (strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0);
        bool is_mac = tag_id_validator_validate(cmd_buf);
        
        if (is_tag_id) {
            // 处理Tag ID
            char normalized_tag[TAG_ID_STR_LEN];
            strncpy(normalized_tag, cmd_buf, TAG_ID_STR_LEN - 1);
            normalized_tag[TAG_ID_STR_LEN - 1] = '\0';
            tag_id_validator_normalize(normalized_tag);
            
            if (!tag_id_validator_validate(normalized_tag)) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID format.\r\n", 33);
                uart_write_bytes(UART_NUM_0, "Expected: 0x0001-0xFFFF, got: ", 31);
                uart_write_bytes(UART_NUM_0, cmd_buf, strlen(cmd_buf));
                uart_write_bytes(UART_NUM_0, "\r\n", 2);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
                return;
            }
            
            // 检查Tag ID是否已存在
            asset_record_t existing;
            esp_err_t ret = asset_load(normalized_tag, &existing);
            
            if (ret == ESP_OK && existing.is_valid) {
                // Tag ID已存在，进入验证更新流程
                handle_verification_update(normalized_tag, &existing);
                return;
            }
            
            // Tag ID不存在，进入注册流程
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized_tag);
            g_camera_state = CAM_STATE_WAITING_REG_NAME;
            show_registration_name_guide(normalized_tag);
            return;
            
        } else if (is_mac) {
            // 处理MAC地址
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "0X%04X", 
                     (unsigned int)(cmd_buf[0] << 8 | cmd_buf[1]));
            ESP_LOGW(TAG, "Legacy MAC format detected, converting to Tag ID: %s", g_current_tag_id);
            g_camera_state = CAM_STATE_WAITING_REG_NAME;
            show_registration_name_guide(cmd_buf);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID format.\r\n", 33);
            uart_write_bytes(UART_NUM_0, "Expected: 0x0001-0xFFFF\r\n", 24);
            uart_write_bytes(UART_NUM_0, "Example: 0x0001, 0x00AB, 0xABCD\r\n", 32);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
            return;
        }
    }
    
    // ========== 5b. 等待注册名称 ===========
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
    
    // ========== 5c. 等待注册区域 ===========
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
    
    // ========== 5d. 等待注册数量 ===========
    if (g_camera_state == CAM_STATE_WAITING_REG_QUANTITY) {
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
            
            extern bool g_is_inventory_mode;
            extern bool g_is_outbound_mode;
            g_is_inventory_mode = false;
            g_is_outbound_mode = false;
            g_inventory_state = INVENTORY_IDLE;
            
            extern int g_total_views;
            g_total_views = 3;
            
            extern void led_camera_registration(void);
            led_camera_registration();
            
            char summary[384];
            snprintf(summary, sizeof(summary),
                     "\r\n========== REGISTRATION SUMMARY ==========\r\n"
                     "  Tag ID:       %s\r\n"
                     "  Item Name:    %s\r\n"
                     "  Storage Area: %c\r\n"
                     "  Quantity:     %lu\r\n"
                     "===========================================\r\n"
                     "[SYSTEM] Initializing camera...\r\n",
                     g_current_tag_id, g_reg_item_name, g_reg_storage_area, (unsigned long)g_reg_quantity);
            uart_write_bytes(UART_NUM_0, summary, strlen(summary));
            
            handle_tag_id_initialization(g_current_tag_id);
            show_registration_step1(g_current_tag_id);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a valid positive integer.\r\n", 45);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity: ", 24);
            return;
        }
    }
    
    // ========== 5e. 等待出库Tag ID ===========
    if (g_camera_state == CAM_STATE_WAITING_OUT_TAG_ID) {
        // 处理Tag ID或MAC地址
        bool is_tag_id = (strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0);
        
        char lookup_id[TAG_ID_STR_LEN] = {0};
        if (is_tag_id) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID format.\r\n", 33);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
                return;
            }
        } else if (tag_id_validator_validate(cmd_buf)) {
            snprintf(lookup_id, TAG_ID_STR_LEN, "%s", cmd_buf);
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID or MAC format.\r\n", 39);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
            return;
        }
        
        // 查找资产记录
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) {
            uart_write_bytes(UART_NUM_0, "[ERROR] Memory allocation failed\r\n", 33);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }

        esp_err_t ret = asset_load(lookup_id, record);
        if (ret == ESP_OK) {
            char info_msg[320];
            snprintf(info_msg, sizeof(info_msg),
                     "\r\n========== OUTBOUND MODE ==========\r\n"
                     "  Tag ID: %s\r\n"
                     "  Item:   %s\r\n"
                     "  Area:   %c\r\n"
                     "  Stock:  %lu\r\n"
                     "===================================\r\n",
                     lookup_id, record->item_name, record->storage_area,
                     (unsigned long)record->quantity);
            uart_write_bytes(UART_NUM_0, info_msg, strlen(info_msg));
            
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            free(record);
            
            g_camera_state = CAM_STATE_WAITING_OUT_QTY;
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to remove: ", 34);
            return;
        } else {
            free(record);
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "\r\n[ERROR] Asset not found: %s\r\n", lookup_id);
            uart_write_bytes(UART_NUM_0, err_msg, strlen(err_msg));
            uart_write_bytes(UART_NUM_0, "Please register this asset first.\r\n", 36);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }
    }
    
    // ========== 5f. 等待出库数量 ===========
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
            
            extern bool g_is_inventory_mode;
            extern bool g_is_outbound_mode;
            g_is_inventory_mode = false;
            g_is_outbound_mode = true;
            g_inventory_state = INVENTORY_IDLE;
            
            extern int g_total_views;
            g_total_views = 1;
            
            extern void led_camera_inventory(void);
            led_camera_inventory();
            
            char summary[320];
            snprintf(summary, sizeof(summary),
                     "\r\n========== OUTBOUND ============\r\n"
                     "  Tag ID:    %s\r\n"
                     "  Remove:    %lu\r\n"
                     "  [STEP 1/1] Capture FRONT view\r\n"
                     "           -> Send 'f' to capture\r\n"
                     "====================================\r\n",
                     g_current_tag_id, (unsigned long)g_outbound_quantity);
            uart_write_bytes(UART_NUM_0, summary, strlen(summary));
            
            handle_tag_id_initialization(g_current_tag_id);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a valid positive integer.\r\n", 45);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to remove: ", 34);
            return;
        }
    }
    
    // ========== 5g. 验证更新 - 等待捕获 ===========
    if (g_camera_state == CAM_STATE_VERIFYING_EXISTING) {
        if (strcasecmp(cmd_buf, "f") == 0) {
            // 捕获验证图像            g_camera_state = CAM_STATE_WAITING_VERIFY_CAPTURE;
            
            // 捕获图像并处理
            extern bool g_camera_power_on;
            extern SemaphoreHandle_t xCameraMutex;
            if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                float verify_feature[FEATURE_VEC_SIZE] = {0};
                
                // 捕获并处理图像
                extern bool camera_module_capture_and_process(float *feature, int size);
                if (camera_module_capture_and_process(verify_feature, FEATURE_VEC_SIZE)) {
                    // 执行验证
                    extern verify_context_t g_verify_ctx;
                    verify_output_t output;
                    bool verify_ok = verify_handler_execute(&g_verify_ctx, verify_feature, 0, &output);
                    
                    if (verify_ok && output.result == VERIFY_RESULT_MATCH) {
                        // 验证通过
                        show_verification_add_qty_guide(g_current_tag_id,
                            g_verify_ctx.existing_record->item_name,
                            g_verify_ctx.existing_record->quantity);
                        g_camera_state = CAM_STATE_WAITING_REG_ADD_QTY;
                    } else if (verify_ok && output.result == VERIFY_RESULT_LOW_CONF) {
                        show_verification_failed(output.confidence, output.threshold_used);
                        show_verification_retry_guide();
                        g_camera_state = CAM_STATE_VERIFYING_EXISTING;
                    } else {
                        show_verification_failed(output.confidence, output.threshold_used);
                        if (!verify_handler_is_max_retries_reached(&g_verify_ctx)) {
                            show_verification_retry_guide();
                            g_camera_state = CAM_STATE_VERIFYING_EXISTING;
                        } else {
                            uart_write_bytes(UART_NUM_0, 
                                "\r\n[ERROR] Max retries reached. Operation cancelled.\r\n", 54);
                            verify_handler_reset(&g_verify_ctx);
                            g_camera_state = CAM_STATE_WAITING_TAG_ID;
                            show_main_menu();
                        }
                    }
                } else {
                    uart_write_bytes(UART_NUM_0, "[ERROR] Capture failed. Please try again.\r\n", 43);
                    g_camera_state = CAM_STATE_VERIFYING_EXISTING;
                }
                xSemaphoreGive(xCameraMutex);
            } else {
                uart_write_bytes(UART_NUM_0, "[ERROR] Camera mutex timeout.\r\n", 32);
                g_camera_state = CAM_STATE_VERIFYING_EXISTING;
            }
            return;
        } else if (strcasecmp(cmd_buf, "q") == 0) {
            // 取消验证
            extern verify_context_t g_verify_ctx;
            verify_handler_reset(&g_verify_ctx);
            uart_write_bytes(UART_NUM_0, "\r\n[INFO] Verification cancelled.\r\n", 35);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[GUIDE] Send 'f' to capture and verify, or 'q' to cancel: ", 57);
            return;
        }
    }
    
    // ========== 5h. 验证通过 - 等待输入数量 ===========
    if (g_camera_state == CAM_STATE_WAITING_REG_ADD_QTY) {
        if (strcasecmp(cmd_buf, "q") == 0) {
            extern verify_context_t g_verify_ctx;
            verify_handler_reset(&g_verify_ctx);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }
        
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) {
            if (!isdigit((unsigned char)cmd_buf[i])) {
                valid = false;
                break;
            }
        }
        
        if (valid && strlen(cmd_buf) > 0) {
            uint32_t add_qty = (uint32_t)atoi(cmd_buf);
            if (add_qty == 0) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Quantity must be > 0.\r\n", 31);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to ADD: ", 31);
                return;
            }
            
            // 更新数量
            asset_record_t update_record;
            extern verify_context_t g_verify_ctx;
            if (g_verify_ctx.existing_record) {
                memcpy(&update_record, g_verify_ctx.existing_record, sizeof(update_record));
                
                // 检查溢出
                if (update_record.quantity > UINT32_MAX - add_qty) {
                    uart_write_bytes(UART_NUM_0, "[ERROR] Quantity overflow! Cannot exceed UINT32_MAX.\r\n", 53);
                    g_camera_state = CAM_STATE_WAITING_TAG_ID;
                    show_main_menu();
                    return;
                }
                
                update_record.quantity += add_qty;
                
                bool is_overwrite = false;
                esp_err_t ret = asset_save(&update_record, &is_overwrite);
                
                if (ret == ESP_OK) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "\r\nâ?QUANTITY UPDATED!\r\n"
                             "  Tag ID: %s\r\n"
                             "  Item:   %s\r\n"
                             "  Old Qty: %lu\r\n"
                             "  Added:   %lu\r\n"
                             "  New Qty: %lu\r\n"
                             "\r\n",
                             g_current_tag_id, update_record.item_name,
                             (unsigned long)(update_record.quantity - add_qty),
                             (unsigned long)add_qty,
                             (unsigned long)update_record.quantity);
                    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
                } else {
                    uart_write_bytes(UART_NUM_0, "\r\nâ?FAILED TO UPDATE QUANTITY\r\n", 33);
                }
            }
            
            verify_handler_reset(&g_verify_ctx);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Please input a valid positive integer.\r\n", 45);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input quantity to ADD: ", 31);
            return;
        }
    }
    
    // ========== 6. 等待库存Tag ID ===========
    if (g_camera_state == CAM_STATE_WAITING_INV_TAG_ID) {
        char lookup_id[TAG_ID_STR_LEN] = {0};
        bool is_tag_id = (strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0);
        
        if (is_tag_id) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID format.\r\n", 33);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
                return;
            }
        } else if (tag_id_validator_validate(cmd_buf)) {
            snprintf(lookup_id, TAG_ID_STR_LEN, "%s", cmd_buf);
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID or MAC format.\r\n", 39);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
            return;
        }
        
        extern bool g_is_inventory_mode;
        g_is_inventory_mode = true;
        extern int g_total_views;
        g_total_views = 3;
        
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) {
            uart_write_bytes(UART_NUM_0, "[ERROR] Memory allocation failed\r\n", 33);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }

        esp_err_t ret = asset_load(lookup_id, record);

        if (ret == ESP_OK) {
            memcpy(g_front_feature, record->front_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(g_side_feature, record->side_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(g_top_feature, record->top_feature, FEATURE_VEC_SIZE * sizeof(float));
            free(record);
            
            extern void led_camera_inventory(void);
            led_camera_inventory();
            
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            ESP_LOGI(TAG, "Starting inventory for Tag ID: %s", lookup_id);
            handle_tag_id_initialization(lookup_id);
            g_inventory_state = INVENTORY_WAITING_FRONT;
            show_inventory_step1(lookup_id);
            return;
        } else {
            free(record);
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "\r\n[ERROR] Asset not found: %s\r\n", lookup_id);
            uart_write_bytes(UART_NUM_0, err_msg, strlen(err_msg));
            uart_write_bytes(UART_NUM_0, "Please register this asset first.\r\n", 36);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }
    }
    
    // ========== 7. 等待删除Tag ID ===========
    if (g_camera_state == CAM_STATE_WAITING_DEL_TAG_ID) {
        char lookup_id[TAG_ID_STR_LEN] = {0};
        bool is_tag_id = (strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0);
        
        if (is_tag_id) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID format.\r\n", 33);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
                return;
            }
        } else if (tag_id_validator_validate(cmd_buf)) {
            snprintf(lookup_id, TAG_ID_STR_LEN, "%s", cmd_buf);
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID or MAC format.\r\n", 39);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
            return;
        }
        
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) {
            uart_write_bytes(UART_NUM_0, "[ERROR] Memory allocation failed\r\n", 33);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }
        
        esp_err_t ret = asset_load(lookup_id, record);
        if (ret == ESP_OK) {
            char confirm_msg[256];
            snprintf(confirm_msg, sizeof(confirm_msg),
                     "\r\nâ ď¸  CONFIRM DELETE ASSET?\r\n"
                     "  Tag ID: %s\r\n"
                     "  Item:   %s\r\n"
                     "  Press 'y' to confirm, any other key to cancel: ",
                     lookup_id, record->item_name);
            uart_write_bytes(UART_NUM_0, confirm_msg, strlen(confirm_msg));
            
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            g_camera_state = CAM_STATE_WAITING_DEL_CONFIRM;
            free(record);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "\r\nâ?ASSET NOT FOUND\r\n", 24);
            uart_write_bytes(UART_NUM_0, "Asset ", 6);
            uart_write_bytes(UART_NUM_0, lookup_id, strlen(lookup_id));
            uart_write_bytes(UART_NUM_0, " does not exist.\r\n\r\n", 21);
            
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            free(record);
            return;
        }
    }
    
    // ========== 8. 等待删除确认 ===========
    if (g_camera_state == CAM_STATE_WAITING_DEL_CONFIRM) {
        if (strcasecmp(cmd_buf, "y") == 0) {
            esp_err_t ret = asset_delete(g_current_tag_id);
            if (ret == ESP_OK) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "\r\nâ?ASSET DELETED SUCCESSFULLY!\r\n"
                         "  Tag ID: %s has been removed.\r\n\r\n", g_current_tag_id);
                uart_write_bytes(UART_NUM_0, msg, strlen(msg));
                
                extern void asset_list_uart(void);
                asset_list_uart();
            } else {
                uart_write_bytes(UART_NUM_0, "\r\nâ?FAILED TO DELETE ASSET\r\n", 27);
                uart_write_bytes(UART_NUM_0, "An error occurred during deletion.\r\n\r\n", 39);
            }
        } else {
            uart_write_bytes(UART_NUM_0, "\r\nâ?DELETION CANCELLED\r\n", 25);
            uart_write_bytes(UART_NUM_0, "Asset was not deleted.\r\n\r\n", 28);
        }
        
        g_camera_state = CAM_STATE_WAITING_TAG_ID;
        show_main_menu();
        return;
    }
    
    // ========== 9. READY状态下的命令处理 ===========
    if (g_camera_state == CAM_STATE_READY) {
        if (strlen(cmd_buf) == 1) {
            char view_cmd = tolower(cmd_buf[0]);
            
            system_msg_t msg = {0};
            snprintf(msg.tag_id, sizeof(msg.tag_id), "%s", g_current_tag_id);
            
            if (view_cmd == 'f') {
                msg.cmd = CMD_CAPTURE_FRONT;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                
                if (g_inventory_state == INVENTORY_WAITING_FRONT || g_inventory_state == INVENTORY_IDLE) {
                    if (g_inventory_state == INVENTORY_IDLE) {
                        show_registration_step2();
                    } else {
                        show_inventory_step2();
                    }
                }
            } else if (view_cmd == 's') {
                msg.cmd = CMD_CAPTURE_SIDE;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
                
                if (g_inventory_state == INVENTORY_IDLE) {
                    show_registration_step3();
                } else {
                    show_inventory_step3();
                }
            } else if (view_cmd == 't') {
                msg.cmd = CMD_CAPTURE_TOP;
                xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
            } else {
                uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
            }
            return;
        }
    }
    
    // ========== 10. 未知命令 ==========
    uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help' for assistance.\r\n", 53);
}

