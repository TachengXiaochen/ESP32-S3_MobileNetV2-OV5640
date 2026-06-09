#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "uart_handler_0.h"
#include "main.h"
#include "modules/system/verify/tag_id_validator.h"
#include "modules/system/storage/asset_manager.h"
#include "modules/system/verify/verify_handler.h"
#include "modules/camera/camera_module.h"
#include "modules/ai/ai_module.h"

static const char *TAG = "uart0_handler";

// ===== UI 函数（从 cmd_handler.c 迁移）=====

// ⭐ 验证式更新：显示现有资产信息
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

// ⭐ 验证通过：提示输入累加数量
void show_verification_add_qty_guide(const char *tag_id, const char *item_name, uint32_t current_qty)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\nVERIFICATION PASSED!\r\n"
             "  Tag ID: %s\r\n  Item:   %s\r\n  Stock:  %lu\r\n"
             "\r\n[GUIDE] Input quantity to ADD (or 'q' to cancel): ",
             tag_id, item_name, (unsigned long)current_qty);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

// ⭐ 验证失败
void show_verification_failed(float confidence, float threshold)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "\r\nVERIFICATION FAILED!\r\n  Similarity: %.1f%% (threshold: %.0f%%)\r\n"
             "  Item mismatch! Quantity NOT updated.\r\n\r\n",
             confidence * 100.0f, threshold * 100.0f);
    uart_write_bytes(UART_NUM_0, msg, strlen(msg));
}

// ⭐ 验证重试提示
void show_verification_retry_guide(void)
{
    const char *guide =
        "\r\n[GUIDE] Send 'f' to retry capture and verify,\r\n"
        "        or 'q' to cancel: ";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void cmd_handler_show_help(void)
{
    const char *help_text =
        "\r\n[HELP] Command List:\r\n"
        "  Tag ID (新注册):   0x0001-0xFFFF\r\n"
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

void show_registration_step1(const char *tag_id)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== REGISTRATION ==========\r\n"
             "  Target: %s\r\n  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view -> Send 'f'\r\n"
             "====================================\r\n", tag_id);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void show_registration_step2(void)
{
    const char *guide = "\r\n[STEP 2/3] Capture SIDE view -> Send 's'\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void show_registration_step3(void)
{
    const char *guide = "\r\n[STEP 3/3] Capture TOP view -> Send 't' to save\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void show_inventory_step1(const char *tag_id)
{
    char guide[256];
    snprintf(guide, sizeof(guide),
             "\r\n========== INVENTORY ============\r\n"
             "  Target: %s\r\n  Camera: POWER ON\r\n"
             "  [STEP 1/3] Capture FRONT view -> Send 'f'\r\n"
             "====================================\r\n", tag_id);
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void show_inventory_step2(void)
{
    const char *guide = "\r\n[STEP 2/3] Capture SIDE view -> Send 's'\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

void show_inventory_step3(void)
{
    const char *guide = "\r\n[STEP 3/3] Capture TOP view -> Send 't' to analyze\r\n";
    uart_write_bytes(UART_NUM_0, guide, strlen(guide));
}

// ===== UART0 接收任务 =====
static void uart0_dispatch(const char *cmd_line);
static void uart0_recv_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    uint8_t *data = (uint8_t *)malloc(1024 * 2);
    char line_buf[128] = {0};
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM_0, data, 1024 * 2, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t ch = data[i];
                if (ch == '\r' || ch == '\n') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received: %s", line_buf);
                        uart0_dispatch(line_buf);
                        line_pos = 0;
                        memset(line_buf, 0, sizeof(line_buf));
                    }
                } else {
                    if (line_pos < sizeof(line_buf) - 1)
                        line_buf[line_pos++] = ch;
                }
            }
        }
        esp_task_wdt_reset();
    }
    free(data);
    vTaskDelete(NULL);
}

// ===== 命令分发（完全替代 cmd_handler_process）=====
#include <stdlib.h>
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static void uart0_dispatch(const char *cmd_line)
{
    if (cmd_line == NULL || strlen(cmd_line) == 0) return;

    const char *cmd = cmd_line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\t')) len--;

    char cmd_buf[128];
    strncpy(cmd_buf, cmd, MIN(len, sizeof(cmd_buf) - 1));
    cmd_buf[MIN(len, sizeof(cmd_buf) - 1)] = '\0';

    // 退出命令
    if (strcasecmp(cmd_buf, "exit") == 0 || strcasecmp(cmd_buf, "quit") == 0) {
        uart_write_bytes(UART_NUM_0, "\r\n[EXIT] Returning to main menu...\r\n", 38);
        g_camera_state = CAM_STATE_WAITING_TAG_ID;
        g_view_state = BE_VIEW_NONE;
        g_inventory_state = INVENTORY_IDLE;
        g_is_inventory_mode = false;
        g_is_outbound_mode = false;
        inference_job_t discard_job;
        while (xQueueReceive(xInferenceQueue, &discard_job, 0) == pdTRUE) {}
        g_views_enqueued = 0; g_views_processed = 0; g_total_views = 0;
        if (g_camera_power_on) {
            extern void led_camera_off(void);
            led_camera_off();
            g_camera_power_on = false;
            system_msg_t deinit_msg = { .cmd = CMD_DEINIT_CAMERA };
            xQueueSend(xSystemQueue, &deinit_msg, pdMS_TO_TICKS(500));
            uart_write_bytes(UART_NUM_0, "Camera: POWER OFF\r\n", 21);
        }
        show_main_menu();
        return;
    }

    // 帮助
    if (strcasecmp(cmd_buf, "help") == 0 || strcmp(cmd_buf, "?") == 0) {
        cmd_handler_show_help();
        return;
    }

    // 列表/信息
    if (strcasecmp(cmd_buf, "l") == 0 || strcasecmp(cmd_buf, "list") == 0) {
        extern void asset_list_uart(void);
        asset_list_uart();
        return;
    }
    if (strcasecmp(cmd_buf, "i") == 0 || strcasecmp(cmd_buf, "info") == 0) {
        extern void print_system_info_uart(void);
        print_system_info_uart();
        return;
    }

    // 主菜单命令
    if (g_camera_state == CAM_STATE_WAITING_TAG_ID) {
        if (strcasecmp(cmd_buf, "r") == 0) {
            ESP_LOGI(TAG, "Entering registration mode");
            g_camera_state = CAM_STATE_WAITING_REG_TAG_ID;
            g_inventory_state = INVENTORY_IDLE;
            uart_write_bytes(UART_NUM_0, "\r\n========== REGISTRATION MODE ==========\r\n"
                "  Input Tag ID (0x0001-0xFFFF): ", 61);
            return;
        }
        if (strcasecmp(cmd_buf, "c") == 0) {
            ESP_LOGI(TAG, "Entering inventory mode");
            g_camera_state = CAM_STATE_WAITING_INV_TAG_ID;
            uart_write_bytes(UART_NUM_0, "\r\n========== INVENTORY MODE ==========\r\n"
                "  Input Tag ID (0x0001-0xFFFF): ", 59);
            return;
        }
        if (strcasecmp(cmd_buf, "o") == 0) {
            ESP_LOGI(TAG, "Entering outbound mode");
            g_camera_state = CAM_STATE_WAITING_OUT_TAG_ID;
            g_inventory_state = INVENTORY_IDLE;
            uart_write_bytes(UART_NUM_0, "\r\n========== OUTBOUND MODE ==========\r\n"
                "  Input Tag ID (0x0001-0xFFFF): ", 58);
            return;
        }
        if (strcasecmp(cmd_buf, "d") == 0) {
            ESP_LOGI(TAG, "Entering delete mode");
            g_camera_state = CAM_STATE_WAITING_DEL_TAG_ID;
            extern void asset_list_uart(void);
            asset_list_uart();
            uart_write_bytes(UART_NUM_0, "\r\n========== DELETE MODE ==========\r\n"
                "  Input Tag ID (0x0001-0xFFFF): ", 56);
            return;
        }
        uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help'.\r\n", 41);
        return;
    }

    // 注册流程：等待 Tag ID
    if (g_camera_state == CAM_STATE_WAITING_REG_TAG_ID) {
        bool is_tag_id = (strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0);
        if (is_tag_id) {
            char normalized[TAG_ID_STR_LEN];
            strncpy(normalized, cmd_buf, TAG_ID_STR_LEN - 1);
            normalized[TAG_ID_STR_LEN - 1] = '\0';
            tag_id_validator_normalize(normalized);
            if (!tag_id_validator_validate(normalized)) {
                uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID. Expected 0x0001-0xFFFF\r\n", 48);
                uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
                return;
            }
            // 检查资产是否存在（堆分配避免栈溢出，asset_record_t ~15KB）
            asset_record_t *existing = (asset_record_t *)malloc(sizeof(asset_record_t));
            if (!existing) { uart_write_bytes(UART_NUM_0, "[ERROR] OOM\r\n", 13); return; }
            bool found = (asset_load(normalized, existing) == ESP_OK && existing->is_valid);
            if (found) {
                // 验证式更新流程
                snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);
                show_verification_existing_guide(normalized, existing->item_name,
                                                  existing->storage_area, existing->quantity);
                free(existing);
                g_camera_state = CAM_STATE_VERIFYING_EXISTING;
                // verify_handler_start 需要 existing_record，重载一次
                asset_record_t *rec2 = (asset_record_t *)malloc(sizeof(asset_record_t));
                if (rec2) {
                    asset_load(normalized, rec2);
                    verify_handler_start(normalized, rec2, VERIFY_MODE_FAST, &g_verify_ctx);
                }
                ai_module_init();
                system_msg_t init_msg = {0};
                init_msg.cmd = CMD_INIT_CAMERA;
                snprintf(init_msg.tag_id, sizeof(init_msg.tag_id), "%s", normalized);
                xQueueSend(xSystemQueue, &init_msg, portMAX_DELAY);
                return;
            }
            free(existing);
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);
            g_camera_state = CAM_STATE_WAITING_REG_NAME;
            uart_write_bytes(UART_NUM_0, "\r\n[STEP 2/4] Input item name: ", 30);
            return;
        } else {
            uart_write_bytes(UART_NUM_0, "[ERROR] Invalid format. Use 0x0001-0xFFFF\r\n", 43);
            uart_write_bytes(UART_NUM_0, "[GUIDE] Input Tag ID: ", 22);
            return;
        }
    }

    // 注册：等待名称
    if (g_camera_state == CAM_STATE_WAITING_REG_NAME) {
        if (strlen(cmd_buf) > 0 && strlen(cmd_buf) < 128) {
            snprintf(g_reg_item_name, 128, "%s", cmd_buf);
            g_camera_state = CAM_STATE_WAITING_REG_AREA;
            uart_write_bytes(UART_NUM_0, "\r\n[STEP 3/4] Input storage area (A-Z): ", 38);
            return;
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Name 1-127 chars.\r\n[GUIDE] Input item name: ", 53); return; }
    }

    // 注册：等待区域
    if (g_camera_state == CAM_STATE_WAITING_REG_AREA) {
        if (strlen(cmd_buf) == 1 && isalpha((unsigned char)cmd_buf[0])) {
            g_reg_storage_area = toupper((unsigned char)cmd_buf[0]);
            g_camera_state = CAM_STATE_WAITING_REG_QUANTITY;
            uart_write_bytes(UART_NUM_0, "\r\n[STEP 4/4] Input quantity: ", 28);
            return;
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Single letter A-Z.\r\n[GUIDE] Input area: ", 48); return; }
    }

    // 注册：等待数量
    if (g_camera_state == CAM_STATE_WAITING_REG_QUANTITY) {
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) if (!isdigit((unsigned char)cmd_buf[i])) { valid = false; break; }
        if (valid && strlen(cmd_buf) > 0) {
            g_reg_quantity = (uint32_t)atoi(cmd_buf);
            if (g_reg_quantity == 0) { uart_write_bytes(UART_NUM_0, "[ERROR] Qty > 0.\r\n[GUIDE] Input quantity: ", 44); return; }
            g_is_inventory_mode = false;
            g_is_outbound_mode = false;
            g_inventory_state = INVENTORY_IDLE;
            g_total_views = 3;
            extern void led_camera_registration(void);
            led_camera_registration();
            char summary[384];
            snprintf(summary, sizeof(summary),
                     "\r\n========== REG SUMMARY ==========\r\n  Tag ID: %s\r\n  Item: %s\r\n  Area: %c\r\n  Qty: %lu\r\n"
                     "==================================\r\n[SYSTEM] Initializing...\r\n",
                     g_current_tag_id, g_reg_item_name, g_reg_storage_area, (unsigned long)g_reg_quantity);
            uart_write_bytes(UART_NUM_0, summary, strlen(summary));
            // 业务逻辑委托给 business_executor
            g_camera_state = CAM_STATE_READY;
            ai_module_init();
            // ⭐ 接入 business_executor 状态机：
            // be_handle_register 会异步初始化摄像头（CMD_INIT_CAMERA），
            // 并设置 g_be_state = WAITING_CAPTURE，后续 f/s/t 命令将被正常接受
            be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_REGISTER, g_current_tag_id, NULL);
            show_registration_step1(g_current_tag_id);
            return;
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Positive integer.\r\n[GUIDE] Input quantity: ", 50); return; }
    }

    // 验证式更新：等待拍摄/取消
    if (g_camera_state == CAM_STATE_VERIFYING_EXISTING) {
        if (strcasecmp(cmd_buf, "f") == 0) {
            g_camera_state = CAM_STATE_WAITING_VERIFY_CAPTURE;
            if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                float verify_feature[FEATURE_VEC_SIZE] = {0};
                if (camera_module_capture_and_process(verify_feature, FEATURE_VEC_SIZE, NULL)) {
                    verify_output_t output;
                    bool verify_ok = verify_handler_execute(&g_verify_ctx, verify_feature, 0, &output);
                    if (verify_ok && output.result == VERIFY_RESULT_MATCH) {
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
                            uart_write_bytes(UART_NUM_0, "\r\n[ERROR] Max retries.\r\n", 22);
                            verify_handler_reset(&g_verify_ctx);
                            g_camera_state = CAM_STATE_WAITING_TAG_ID;
                            show_main_menu();
                        }
                    }
                } else { uart_write_bytes(UART_NUM_0, "[ERROR] Capture failed.\r\n", 25); }
                xSemaphoreGive(xCameraMutex);
            }
            return;
        } else if (strcasecmp(cmd_buf, "q") == 0) {
            verify_handler_reset(&g_verify_ctx);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        } else { uart_write_bytes(UART_NUM_0, "[GUIDE] 'f' to capture, 'q' to cancel: ", 40); return; }
    }

    // 验证通过：等待累加数量
    if (g_camera_state == CAM_STATE_WAITING_REG_ADD_QTY) {
        if (strcasecmp(cmd_buf, "q") == 0) {
            verify_handler_reset(&g_verify_ctx);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            show_main_menu();
            return;
        }
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) if (!isdigit((unsigned char)cmd_buf[i])) { valid = false; break; }
        if (valid && strlen(cmd_buf) > 0) {
            uint32_t add_qty = (uint32_t)atoi(cmd_buf);
            if (add_qty == 0) { uart_write_bytes(UART_NUM_0, "[ERROR] Qty > 0.\r\n", 18); return; }
            if (g_verify_ctx.existing_record) {
                // 堆分配避免栈溢出，asset_record_t ~15KB
                asset_record_t *update_record = (asset_record_t *)malloc(sizeof(asset_record_t));
                if (update_record) {
                    memcpy(update_record, g_verify_ctx.existing_record, sizeof(*update_record));
                    update_record->quantity += add_qty;
                    bool is_overwrite = false;
                    asset_save(update_record, &is_overwrite);
                    free(update_record);
                }
            }
            verify_handler_reset(&g_verify_ctx);
            g_camera_state = CAM_STATE_WAITING_TAG_ID;
            uart_write_bytes(UART_NUM_0, "\r\nQUANTITY UPDATED!\r\n", 22);
            show_main_menu();
            return;
        }
        return;
    }

    // 盘点：等待 Tag ID
    if (g_camera_state == CAM_STATE_WAITING_INV_TAG_ID) {
        char lookup_id[TAG_ID_STR_LEN] = {0};
        if ((strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0)) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) { uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID.\r\n", 25); return; }
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Use 0x0001-0xFFFF format.\r\n", 36); return; }
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) return;
        if (asset_load(lookup_id, record) == ESP_OK) {
            memcpy(g_stored_front_feature, record->front_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(g_stored_side_feature, record->side_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(g_stored_top_feature, record->top_feature, FEATURE_VEC_SIZE * sizeof(float));
            free(record);
            g_is_inventory_mode = true;
            g_total_views = 3;
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            extern void led_camera_inventory(void);
            led_camera_inventory();
            g_inventory_state = INVENTORY_WAITING_FRONT;
            g_camera_state = CAM_STATE_READY;
            ai_module_init();
            system_msg_t init_msg = {0};
            init_msg.cmd = CMD_INIT_CAMERA;
            snprintf(init_msg.tag_id, sizeof(init_msg.tag_id), "%s", lookup_id);
            xQueueSend(xSystemQueue, &init_msg, portMAX_DELAY);
            // 同步 business_executor 状态机，使 be_handle_capture() 接受 f/s/t 命令
            g_be_state = BE_STATE_WAITING_CAPTURE;
            g_be_task = BE_CMD_INVENTORY;
            show_inventory_step1(lookup_id);
            return;
        } else { free(record); uart_write_bytes(UART_NUM_0, "[ERROR] Asset not found.\r\n", 26); return; }
    }

    // 出库：等待 Tag ID
    if (g_camera_state == CAM_STATE_WAITING_OUT_TAG_ID) {
        char lookup_id[TAG_ID_STR_LEN] = {0};
        if ((strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0)) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) { uart_write_bytes(UART_NUM_0, "[ERROR] Invalid Tag ID.\r\n", 25); return; }
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Use 0x0001-0xFFFF format.\r\n", 36); return; }
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) return;
        if (asset_load(lookup_id, record) == ESP_OK) {
            char info_msg[320];
            snprintf(info_msg, sizeof(info_msg),
                     "\r\n========== OUTBOUND ==========\r\n  Tag ID: %s\r\n  Item: %s\r\n  Stock: %lu\r\n"
                     "================================\r\n[GUIDE] Input quantity to remove: ",
                     lookup_id, record->item_name, (unsigned long)record->quantity);
            uart_write_bytes(UART_NUM_0, info_msg, strlen(info_msg));
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            // 保存资产上下文到 business_executor（be_on_all_views_done 需要）
            snprintf(g_be_tag_id, sizeof(g_be_tag_id), "%s", lookup_id);
            snprintf(g_be_item_name, sizeof(g_be_item_name), "%s", record->item_name);
            g_be_storage_area = record->storage_area;
            g_be_quantity = record->quantity;
            free(record);
            g_camera_state = CAM_STATE_WAITING_OUT_QTY;
            return;
        } else { free(record); uart_write_bytes(UART_NUM_0, "[ERROR] Asset not found.\r\n", 26); return; }
    }

    // 出库：等待数量
    if (g_camera_state == CAM_STATE_WAITING_OUT_QTY) {
        bool valid = true;
        for (size_t i = 0; i < strlen(cmd_buf); i++) if (!isdigit((unsigned char)cmd_buf[i])) { valid = false; break; }
        if (valid && strlen(cmd_buf) > 0) {
            g_outbound_quantity = (uint32_t)atoi(cmd_buf);
            if (g_outbound_quantity == 0) { uart_write_bytes(UART_NUM_0, "[ERROR] Qty > 0.\r\n", 18); return; }
            g_is_inventory_mode = false;
            g_is_outbound_mode = true;
            g_total_views = 1;
            g_camera_state = CAM_STATE_READY;
            extern void led_camera_inventory(void);
            led_camera_inventory();
            ai_module_init();
            system_msg_t init_msg = {0};
            init_msg.cmd = CMD_INIT_CAMERA;
            snprintf(init_msg.tag_id, sizeof(init_msg.tag_id), "%s", g_current_tag_id);
            xQueueSend(xSystemQueue, &init_msg, portMAX_DELAY);
            // 同步 business_executor 状态机
            g_be_state = BE_STATE_WAITING_CAPTURE;
            g_be_task = BE_CMD_OUTBOUND;
            g_be_remove_qty = g_outbound_quantity;
            uart_write_bytes(UART_NUM_0, "\r\n[STEP 1/1] Capture FRONT view -> Send 'f'\r\n", 42);
            return;
        }
        return;
    }

    // 删除：等待 Tag ID
    if (g_camera_state == CAM_STATE_WAITING_DEL_TAG_ID) {
        char lookup_id[TAG_ID_STR_LEN] = {0};
        if ((strncmp(cmd_buf, "0x", 2) == 0 || strncmp(cmd_buf, "0X", 2) == 0)) {
            strncpy(lookup_id, cmd_buf, TAG_ID_STR_LEN - 1);
            tag_id_validator_normalize(lookup_id);
            if (!tag_id_validator_validate(lookup_id)) { uart_write_bytes(UART_NUM_0, "[ERROR] Invalid ID.\r\n", 22); return; }
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Use 0x0001-0xFFFF.\r\n", 30); return; }
        // 堆分配检查存在性，asset_record_t ~15KB
        asset_record_t *exists = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!exists) { uart_write_bytes(UART_NUM_0, "[ERROR] OOM\r\n", 13); return; }
        bool found = (asset_load(lookup_id, exists) == ESP_OK);
        free(exists);
        if (found) {
            snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", lookup_id);
            g_camera_state = CAM_STATE_WAITING_DEL_CONFIRM;
            uart_write_bytes(UART_NUM_0, "\r\nPress 'y' to confirm delete, any other key to cancel: ", 55);
            return;
        } else { uart_write_bytes(UART_NUM_0, "[ERROR] Asset not found.\r\n", 26); return; }
    }

    // 删除确认
    if (g_camera_state == CAM_STATE_WAITING_DEL_CONFIRM) {
        if (strcasecmp(cmd_buf, "y") == 0) {
            asset_delete(g_current_tag_id);
            uart_write_bytes(UART_NUM_0, "\r\nASSET DELETED.\r\n", 17);
            extern void asset_list_uart(void);
            asset_list_uart();
        } else { uart_write_bytes(UART_NUM_0, "\r\nDeletion cancelled.\r\n", 22); }
        g_camera_state = CAM_STATE_WAITING_TAG_ID;
        show_main_menu();
        return;
    }

    // READY 状态下的 f/s/t 拍摄命令 → 改为调用 be_execute
    if (g_camera_state == CAM_STATE_READY) {
        if (strlen(cmd_buf) == 1) {
            char view_cmd = tolower(cmd_buf[0]);
            if (view_cmd == 'f') {
                be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_FRONT, g_current_tag_id, NULL);
                if (g_inventory_state == INVENTORY_WAITING_FRONT || g_inventory_state == INVENTORY_IDLE) {
                    if (g_inventory_state == INVENTORY_IDLE) show_registration_step2();
                    else show_inventory_step2();
                }
            } else if (view_cmd == 's') {
                be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_SIDE, g_current_tag_id, NULL);
                if (g_inventory_state == INVENTORY_IDLE) show_registration_step3();
                else show_inventory_step3();
            } else if (view_cmd == 't') {
                be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_TOP, g_current_tag_id, NULL);
            } else {
                uart_write_bytes(UART_NUM_0, "[ERROR] Unknown. Use 'f','s','t'.\r\n", 34);
            }
            return;
        }
    }

    uart_write_bytes(UART_NUM_0, "[ERROR] Unknown command. Type 'help'.\r\n", 41);
}

// ===== 事件回调（文本输出）=====
void uart_handler_0_on_event(be_event_t event, const void *data)
{
    char buf[512];
    switch (event) {
        case BE_EVT_CAPTURE_PROGRESS: {
            const be_capture_progress_t *p = (const be_capture_progress_t *)data;
            snprintf(buf, sizeof(buf), "[STEP %d/%d] %s captured, blur=%.1f\r\n",
                     p->view_index + 1, p->total_steps,
                     p->view_index == 0 ? "Front" : (p->view_index == 1 ? "Side" : "Top"),
                     p->blur_score);
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            break;
        }
        case BE_EVT_TASK_DONE: {
            const be_task_done_t *d = (const be_task_done_t *)data;
            snprintf(buf, sizeof(buf), "\r\nTASK DONE: %s\r\n  Tag ID: %s\r\n  Result: %s\r\n\r\n",
                     d->task == BE_CMD_REGISTER ? "Register" : (d->task == BE_CMD_INVENTORY ? "Inventory" : "Outbound"),
                     d->tag_id, d->result ? d->result : "success");
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            show_main_menu();
            break;
        }
        case BE_EVT_ERROR: {
            const be_error_info_t *e = (const be_error_info_t *)data;
            snprintf(buf, sizeof(buf), "[ERROR] %s\r\n", e->error_msg ? e->error_msg : "Unknown");
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            break;
        }
        default: break;
    }
}

// ===== 初始化 =====
void uart_handler_0_init(void)
{
    xTaskCreate(uart0_recv_task, "uart0_recv_task", 16384, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART0 CLI handler initialized");
}
