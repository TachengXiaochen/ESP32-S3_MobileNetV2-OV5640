/**
 * @file app_handlers.c
 * @brief 系统命令处理 + 摄像头AI主任务 + business_executor 输出回调
 *
 * 从 main.c 拆分出来的内部命令 handler 和任务循环。
 * 生产路径（UART1 JSON）通过 be_execute() → camera_ai_task 分发；
 * 调试路径（UART0 CLI）部分绕过 be_execute，直接操作全局变量。
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"

#include "app_handlers.h"
#include "main.h"
#include "modules/camera/camera_module.h"
#include "modules/system/storage/storage_module.h"
#include "modules/ai/ai_module.h"
#include "modules/system/storage/asset_manager.h"
#include "modules/system/led/led_indicator.h"
#include "modules/ai/feature_processor.h"
#include "modules/ai/blur_detection.h"
#include "modules/system/comm/uart_handler_0.h"
#include "modules/system/comm/uart_handler_1.h"

static const char *TAG = "camera_ai";
#define SAFE_WDT_RESET()    esp_task_wdt_reset()
#define UART_NUM UART_NUM_0

// ========== 前向声明 ==========
static void system_shutdown_camera(void);
static void handle_capture_view(system_msg_t *msg);
static void handle_save_asset(system_msg_t *msg);
static void handle_inventory_analysis(system_msg_t *msg);
static void handle_outbound_analyze(system_msg_t *msg);
static void handle_outbound_update_qty(system_msg_t *msg);
static void handle_inference_trigger(system_msg_t *msg);

// ========== 系统关闭摄像头 ==========
static void system_shutdown_camera(void)
{
    led_camera_off();
    g_camera_power_on = false;
    system_msg_t deinit_msg = { .cmd = CMD_DEINIT_CAMERA };
    xQueueSend(xSystemQueue, &deinit_msg, pdMS_TO_TICKS(500));
    g_camera_state = CAM_STATE_WAITING_TAG_ID;
    g_view_state = BE_VIEW_NONE;
    g_inventory_state = INVENTORY_IDLE;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    show_main_menu();
}

// ========== 视图拍摄处理 ==========
static void handle_capture_view(system_msg_t *msg)
{
    if (!g_camera_ready) { uart_write_bytes(UART_NUM, "Camera not ready!\r\n", 20); return; }
    const char *view_name = NULL, *view_label = NULL;
    if (msg->cmd == CMD_CAPTURE_FRONT) { view_name = "Front"; view_label = "front"; led_capture_front(g_is_inventory_mode); }
    else if (msg->cmd == CMD_CAPTURE_SIDE) { view_name = "Side"; view_label = "side"; led_capture_side(g_is_inventory_mode); }
    else { view_name = "Top"; view_label = "top"; led_capture_top(g_is_inventory_mode); }
    esp_task_wdt_reset();
    bool image_saved = false;
    if ((!g_is_inventory_mode || g_is_outbound_mode) && g_storage_ready) {
        if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            uint8_t *jpeg_buf = NULL; size_t jpeg_len = 0;
            if (camera_module_capture_jpeg(&jpeg_buf, &jpeg_len)) {
                ESP_LOGI(TAG, "JPEG %u bytes", (unsigned)jpeg_len);
                esp_err_t ret = storage_module_save_image(g_current_tag_id, view_label, jpeg_buf, jpeg_len);
                if (ret == ESP_OK) image_saved = true;
                else ESP_LOGW(TAG, "Save image fail 0x%x", ret);
                free(jpeg_buf);
            } else ESP_LOGW(TAG, "JPEG capture fail");
            xSemaphoreGive(xCameraMutex);
        } else ESP_LOGW(TAG, "Mutex timeout JPEG");
    }
    inference_job_t job; memset(&job, 0, sizeof(job));
    job.view_cmd = msg->cmd; snprintf(job.tag_id, sizeof(job.tag_id), "%s", g_current_tag_id);
    job.expected_views = g_total_views; job.is_registration = !g_is_inventory_mode && !g_is_outbound_mode;
    job.must_save_jpeg = !g_is_inventory_mode;
    if (xQueueSend(xInferenceQueue, &job, pdMS_TO_TICKS(100))) { g_views_enqueued++; ESP_LOGI(TAG, "Inference enqueued %s %d/%d", view_name, g_views_enqueued, g_total_views); }
    else ESP_LOGW(TAG, "Inference queue full drop %s", view_name);
    char lm[128]; snprintf(lm, sizeof(lm), "%s view captured%s\r\n", view_name, image_saved ? " (img)" : "");
    uart_write_bytes(UART_NUM, lm, strlen(lm));
    esp_task_wdt_reset();
    if (msg->cmd == CMD_CAPTURE_FRONT) {
        g_view_state = BE_VIEW_FRONT;
        if (g_inventory_state == INVENTORY_IDLE) { g_inventory_state = INVENTORY_WAITING_SIDE; show_registration_step2(); }
        else if (g_inventory_state == INVENTORY_WAITING_FRONT) { g_inventory_state = INVENTORY_WAITING_SIDE; show_inventory_step2(); }
    } else if (msg->cmd == CMD_CAPTURE_SIDE) {
        g_view_state = BE_VIEW_SIDE;
        if (g_inventory_state == INVENTORY_WAITING_SIDE) { g_inventory_state = INVENTORY_WAITING_TOP; show_registration_step3(); }
    } else g_view_state = BE_VIEW_TOP;
    // be_on_view_captured 已移至 inference_task.c，在推理完成后调用（此时 blur_score 真实可用）
}

// ========== 资产保存处理 ==========
static void handle_save_asset(system_msg_t *msg)
{
    if (!g_storage_ready || !msg->data) return;
    ESP_LOGI(TAG, "Save asset %s", msg->tag_id);
    save_result_t result = storage_module_save_asset((asset_record_t*)msg->data);
    if (result != SAVE_RESULT_FAILED) {
        uart_write_bytes(UART_NUM, "\r\nREGISTRATION COMPLETE!\r\n", 26);
        uart_write_bytes(UART_NUM, "  Asset saved.\r\n", 18);
        char buf[64]; snprintf(buf, sizeof(buf), "  Tag ID: %s\r\n", msg->tag_id);
        uart_write_bytes(UART_NUM, buf, strlen(buf));
        uart_write_bytes(UART_NUM, "  Camera: POWER OFF\r\n\r\n", 25);
        free(msg->data); system_shutdown_camera();
    } else { uart_write_bytes(UART_NUM, "\r\nFAILED TO SAVE\r\n", 19); }
}

// ========== 盘点分析处理 ==========
static void handle_inventory_analysis(system_msg_t *msg)
{
    ESP_LOGI(TAG, "Inventory analysis for %s", msg->tag_id);
    asset_record_t *r = (asset_record_t*)malloc(sizeof(asset_record_t));
    if (!r) return;
    if (asset_load(msg->tag_id, r) != ESP_OK) { free(r); return; }
    similarity_result_t fr = {0}, sr = {0}, tr = {0};
    ai_module_match_features(g_front_feature, r->front_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &fr);
    ai_module_match_features(g_side_feature, r->side_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &sr);
    ai_module_match_features(g_top_feature, r->top_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &tr);
    float wc = fr.confidence*0.5f + sr.confidence*0.3f + tr.confidence*0.2f;
    char buf[512]; snprintf(buf, sizeof(buf),
        "\r\nINVENTORY: w=%.4f th=%.2f %s\r\n", wc, fr.match_threshold,
        (wc >= fr.match_threshold) ? "MATCH" : "NO MATCH");
    uart_write_bytes(UART_NUM, buf, strlen(buf));
    free(r); system_shutdown_camera();
}

// ========== 出库分析处理 ==========
static void handle_outbound_analyze(system_msg_t *msg)
{
    ESP_LOGI(TAG, "Outbound analyze %s", msg->tag_id);
    asset_record_t *r = (asset_record_t*)malloc(sizeof(asset_record_t));
    if (!r) return;
    if (asset_load(msg->tag_id, r) != ESP_OK) { free(r); return; }
    g_outbound_original_qty = r->quantity;
    similarity_result_t o = {0};
    ai_module_match_features(g_front_feature, r->front_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &o);
    if (o.confidence >= o.match_threshold) {
        system_msg_t um = {0}; um.cmd = CMD_OUTBOUND_UPDATE_QTY;
        snprintf(um.tag_id, sizeof(um.tag_id), "%s", msg->tag_id);
        xQueueSend(xSystemQueue, &um, portMAX_DELAY);
    } else { uart_write_bytes(UART_NUM, "\r\nOUTBOUND FAILED\r\n", 20); system_shutdown_camera(); }
    free(r);
}

// ========== 出库数量更新处理 ==========
static void handle_outbound_update_qty(system_msg_t *msg)
{
    ESP_LOGI(TAG, "Update qty %s", msg->tag_id);
    asset_record_t *u = (asset_record_t*)malloc(sizeof(asset_record_t));
    if (!u) return;
    if (asset_load(msg->tag_id, u) != ESP_OK) { free(u); return; }
    if (g_outbound_quantity >= u->quantity) u->quantity = 0; else u->quantity -= g_outbound_quantity;
    bool ow = false;
    if (u->quantity == 0) { asset_delete(msg->tag_id); uart_write_bytes(UART_NUM, "\r\nOUTBOUND DONE\r\n", 17); }
    else { asset_save(u, &ow); uart_write_bytes(UART_NUM, "\r\nOUTBOUND DONE\r\n", 17); }
    uart_write_bytes(UART_NUM, "  Camera: POWER OFF\r\n\r\n", 25);
    free(u); system_shutdown_camera();
}

// ========== 推理触发处理 ==========
static void handle_inference_trigger(system_msg_t *msg)
{
    bool be_handled = be_on_all_views_done();
    ESP_LOGI(TAG, "All views done");
    if (be_handled) return;  // business_executor 已处理，跳过旧 CLI 路径
    if (g_is_outbound_mode) {
        g_inventory_state = INVENTORY_COMPLETE;
        system_msg_t am = {0}; am.cmd = CMD_OUTBOUND_ANALYZE;
        snprintf(am.tag_id, sizeof(am.tag_id), "%s", g_current_tag_id);
        xQueueSend(xSystemQueue, &am, portMAX_DELAY);
    } else if (g_is_inventory_mode) {
        g_inventory_state = INVENTORY_ANALYZING;
        system_msg_t am = {0}; am.cmd = CMD_START_INVENTORY;
        snprintf(am.tag_id, sizeof(am.tag_id), "%s", g_current_tag_id);
        xQueueSend(xSystemQueue, &am, portMAX_DELAY);
    } else {
        g_inventory_state = INVENTORY_COMPLETE;
        system_msg_t sm = {0}; sm.cmd = CMD_SAVE_ASSET;
        asset_record_t *rec = (asset_record_t*)malloc(sizeof(asset_record_t));
        if (rec) {
            snprintf(rec->tag_id, sizeof(rec->tag_id), "%s", g_current_tag_id);
            snprintf(rec->item_name, sizeof(rec->item_name), "%s", g_reg_item_name);
            rec->storage_area = g_reg_storage_area; rec->quantity = g_reg_quantity;
            memcpy(rec->front_feature, g_front_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(rec->side_feature, g_side_feature, FEATURE_VEC_SIZE * sizeof(float));
            memcpy(rec->top_feature, g_top_feature, FEATURE_VEC_SIZE * sizeof(float));
            rec->is_valid = true; sm.data = rec;
            xQueueSend(xSystemQueue, &sm, portMAX_DELAY);
        }
    }
}

// ========== 摄像头AI主任务 ==========
void camera_ai_task(void *pvParameters)
{
    printf("[DIAG] AI TASK STARTED\n");
    esp_task_wdt_add(NULL);
    system_msg_t msg;

    while (1) {
        if (xQueueReceive(xSystemQueue, &msg, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            printf("[DIAG] AI RCVD msg cmd=%d\n", msg.cmd);
            if (msg.cmd == CMD_INIT_CAMERA) {
                // 摄像头硬件已在 app_main 中一次性初始化，这里只标记就绪
                printf("[DIAG] AI INIT CAMERA (HW initted at boot)\n");
                g_camera_ready = true;
                g_camera_power_on = true;
                uart_write_bytes(UART_NUM, "Camera powered ON\r\n", 20);
            } else if (msg.cmd == CMD_DEINIT_CAMERA) {
                // 摄像头硬件保持初始化状态，仅清除软件标志
                // I2C/DMA 不卸载，避免从任务上下文调用 i2c_driver_delete 崩溃
                printf("[DIAG] AI DEINIT CAMERA (soft flags only)\n");
                g_camera_ready = false;
                g_camera_power_on = false;
                uart_write_bytes(UART_NUM, "Camera powered OFF\r\n", 21);
            } else {
                switch (msg.cmd) {
                    case CMD_CAPTURE_FRONT: case CMD_CAPTURE_SIDE: case CMD_CAPTURE_TOP: handle_capture_view(&msg); break;
                    case CMD_SAVE_ASSET: handle_save_asset(&msg); break;
                    case CMD_START_INVENTORY: handle_inventory_analysis(&msg); break;
                    case CMD_OUTBOUND_ANALYZE: handle_outbound_analyze(&msg); break;
                    case CMD_OUTBOUND_UPDATE_QTY: handle_outbound_update_qty(&msg); break;
                    case CMD_INFERENCE_TRIGGER: handle_inference_trigger(&msg); break;
                    default: ESP_LOGW(TAG, "Unknown cmd %d", msg.cmd); break;
                }
            }
        } else { SAFE_WDT_RESET(); }
    }
}

// ========== business_executor 输出回调 ==========
void be_output_callback(be_channel_t channel, be_event_t event, const void *data)
{
    ESP_LOGI("be_route", "channel=%d event=%d", (int)channel, (int)event);
    if (channel == BE_CHANNEL_UART0_TEXT) uart_handler_0_on_event(event, data);
    else uart_handler_1_on_event(event, data);
}
