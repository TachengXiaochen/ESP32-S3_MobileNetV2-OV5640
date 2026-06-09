#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

#include "modules/camera/camera_module.h"
#include "modules/system/storage/storage_module.h"
#include "modules/ai/ai_module.h"
#include "modules/system/storage/asset_manager.h"
#include "modules/system/led/led_indicator.h"
#include "modules/system/verify/verify_handler.h"
#include "modules/ai/feature_processor.h"
#include "modules/ai/blur_detection.h"
#include "modules/system/executor/business_executor.h"
#include "modules/web/web_server.h"
#include "modules/system/comm/uart_handler_0.h"
#include "modules/system/comm/uart_handler_1.h"
#include "modules/4g/l610_manager.h"
#include "main.h"

#define UART_QUEUE_LEN    10
#define STORAGE_QUEUE_LEN 5

#define UART_NUM UART_NUM_0
#define UART_BAUD_RATE 115200
#define UART_BUF_SIZE (1024 * 2)

static const char *TAG = "camera_ai";
#define SAFE_WDT_RESET()    esp_task_wdt_reset()

QueueHandle_t xSystemQueue = NULL;
QueueHandle_t xStorageQueue = NULL;
QueueHandle_t xInferenceQueue = NULL;
SemaphoreHandle_t xCameraMutex = NULL;

bool g_camera_ready = false;
bool g_storage_ready = false;
float g_front_feature[FEATURE_VEC_SIZE] = {0};
float g_side_feature[FEATURE_VEC_SIZE] = {0};
float g_top_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_front_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_side_feature[FEATURE_VEC_SIZE] = {0};
float g_stored_top_feature[FEATURE_VEC_SIZE] = {0};
inventory_state_t g_inventory_state = INVENTORY_IDLE;
int g_views_enqueued = 0;
int g_views_processed = 0;
int g_total_views = 0;
char g_current_tag_id[TAG_ID_STR_LEN] = {0};
camera_state_t g_camera_state = CAM_STATE_WAITING_TAG_ID;
view_state_t g_view_state = BE_VIEW_NONE;
bool g_camera_power_on = false;
bool g_storage_initialized = false;
bool g_is_inventory_mode = false;
bool g_is_outbound_mode = false;
char g_reg_item_name[128] = {0};
char g_reg_storage_area = 'A';
uint32_t g_reg_quantity = 0;
uint32_t g_outbound_quantity = 0;
uint32_t g_outbound_original_qty = 0;
verify_context_t g_verify_ctx = {0};
char g_l610_client_id[64] = {0};

static void system_shutdown_camera(void);
static void handle_capture_view(system_msg_t *msg);
static void handle_save_asset(system_msg_t *msg);
static void handle_inventory_analysis(system_msg_t *msg);
static void handle_outbound_analyze(system_msg_t *msg);
static void handle_outbound_update_qty(system_msg_t *msg);
static void handle_inference_trigger(system_msg_t *msg);

static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "UART initialized at %d baud", UART_BAUD_RATE);
}

void print_system_info_uart(void)
{
    char info_buf[512];
    const uint32_t free_heap = esp_get_free_heap_size();
    const uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    snprintf(info_buf, sizeof(info_buf),
             "\r\n========== SYSTEM INFO ==========\r\n"
             "  Free Heap: %lu  Min Free: %lu\r\n"
             "  Camera: %s  Storage: %s\r\n"
             "  Tag: %s  Mode: %s\r\n"
             "==================================\r\n",
             (unsigned long)free_heap, (unsigned long)min_free_heap,
             g_camera_ready ? "OK" : "NO", g_storage_ready ? "OK" : "NO",
             strlen(g_current_tag_id) ? g_current_tag_id : "N/A",
             g_is_inventory_mode ? "INV" : "REG");
    uart_write_bytes(UART_NUM, info_buf, strlen(info_buf));
}

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
    int vi = (msg->cmd == CMD_CAPTURE_FRONT) ? 0 : (msg->cmd == CMD_CAPTURE_SIDE) ? 1 : 2;
    be_on_view_captured(vi);
}

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
            memcpy(rec->front_feature, g_front_feature, sizeof(g_front_feature));
            memcpy(rec->side_feature, g_side_feature, sizeof(g_side_feature));
            memcpy(rec->top_feature, g_top_feature, sizeof(g_top_feature));
            rec->is_valid = true; sm.data = rec;
            xQueueSend(xSystemQueue, &sm, portMAX_DELAY);
        }
    }
}

// ========== 摄像头AI主任务 ==========
static void camera_ai_task(void *pvParameters)
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

// ========== 推理任务 ==========
static void inference_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    inference_job_t job;
    const int MAX_FRAMES = 3;              // 边缘情况下最多采3帧（正常1帧）
    while (1) {
        if (xQueueReceive(xInferenceQueue, &job, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            float *fp = NULL; const char *vn = NULL;
            if (job.view_cmd == CMD_CAPTURE_FRONT) { fp = g_front_feature; vn = "Front"; }
            else if (job.view_cmd == CMD_CAPTURE_SIDE) { fp = g_side_feature; vn = "Side"; }
            else { fp = g_top_feature; vn = "Top"; }
            ESP_LOGI(TAG, "[INF] %s start", vn);

            // 自适应帧数：默认1帧，边缘清晰度时补充到最多3帧
            feature_processor_reset_frame_count();
            int fc = 0;
            float blur_var = 0.0f;
            bool need_more = false;

            // --- 第1帧 ---
            if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                float sf[FEATURE_VEC_SIZE];
                if (camera_module_capture_and_process(sf, FEATURE_VEC_SIZE, &blur_var)) {
                    feature_processor_add_frame(sf, FEATURE_VEC_SIZE);
                    fc++;
                    // 判断清晰度：边缘 → 需要补充帧
                    if (blur_var < BLUR_CONFIDENT_THRESHOLD) {
                        need_more = true;
                        ESP_LOGI(TAG, "[INF] %s blur=%.1f marginal, supplementing", vn, (double)blur_var);
                    } else {
                        ESP_LOGI(TAG, "[INF] %s blur=%.1f confident, 1 frame OK", vn, (double)blur_var);
                    }
                }
                xSemaphoreGive(xCameraMutex);
            }
            esp_task_wdt_reset();

            // --- 补充帧（仅边缘情况） ---
            if (need_more) {
                for (int i = 1; i < MAX_FRAMES && fc < MAX_FRAMES; i++) {
                    if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        float sf[FEATURE_VEC_SIZE];
                        if (camera_module_capture_and_process(sf, FEATURE_VEC_SIZE, NULL)) {
                            feature_processor_add_frame(sf, FEATURE_VEC_SIZE);
                            fc++;
                        }
                        xSemaphoreGive(xCameraMutex);
                    }
                    esp_task_wdt_reset();
                }
            }

            // --- 融合输出 ---
            if (fc > 0 && feature_processor_get_fused_feature(fp, FEATURE_VEC_SIZE))
                ESP_LOGI(TAG, "[INF] %s fusion %d frames", vn, fc);
            else ESP_LOGW(TAG, "[INF] %s no frames", vn);

            g_views_processed++;
            if (g_views_processed >= g_total_views) {
                system_msg_t tm = {0}; tm.cmd = CMD_INFERENCE_TRIGGER;
                snprintf(tm.tag_id, sizeof(tm.tag_id), "%s", job.tag_id);
                xQueueSend(xSystemQueue, &tm, portMAX_DELAY);
                g_views_enqueued = 0; g_views_processed = 0; g_total_views = 0;
            }
            esp_task_wdt_reset();
        } else { SAFE_WDT_RESET(); }
    }
}

// ========== 存储管理任务 ==========
static void storage_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    system_msg_t msg;
    while (1) {
        if (xQueueReceive(xStorageQueue, &msg, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            switch (msg.cmd) {
                case CMD_INIT_STORAGE:
                    if (storage_module_init()) { g_storage_ready = true; ESP_LOGI(TAG, "Storage OK"); }
                    else ESP_LOGE(TAG, "Storage FAIL");
                    break;
                default: ESP_LOGW(TAG, "Unknown storage cmd %d", msg.cmd); break;
            }
        } else { SAFE_WDT_RESET(); }
    }
}

// ========== business_executor 输出回调 ==========
static void be_output_callback(be_channel_t channel, be_event_t event, const void *data)
{
    ESP_LOGI("be_route", "channel=%d event=%d", (int)channel, (int)event);
    if (channel == BE_CHANNEL_UART0_TEXT) uart_handler_0_on_event(event, data);
    else uart_handler_1_on_event(event, data);
}

// ========== 主程序入口 ==========
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
    xSystemQueue = xQueueCreate(10, sizeof(system_msg_t));
    xStorageQueue = xQueueCreate(5, sizeof(system_msg_t));
    xInferenceQueue = xQueueCreate(5, sizeof(inference_job_t));
    xCameraMutex = xSemaphoreCreateMutex();
    printf("\n========== MAIN MENU ==========\r\n");
    printf("  r - Register\r\n  o - Outbound\r\n  c - Inventory\r\n  d - Delete\r\n  l - List\r\n  i - Info\r\n  help/? - Menu\r\n===============================\r\n");
    printf("[GUIDE] Select: "); fflush(stdout);
    ESP_LOGI(TAG, "System Ready");
    storage_module_init();
    ai_module_init();
    be_init(be_output_callback);
    // ⭐ 摄像头在 app_main 中一次性初始化（调度器启动前，core 0 上下文）
    // i2c_driver_install 必须在 app_main 上下文调用，不能从 FreeRTOS 任务调用
    // 摄像头硬件保持初始化状态，电源管理通过 GPIO48 单独控制
    if (camera_module_init()) {
        ESP_LOGI(TAG, "Camera initialized at boot");
    } else {
        ESP_LOGE(TAG, "Camera init failed at boot");
    }
    // ⭐ 先在干净的中断环境下创建任务，再初始化 UART（避免 ISR 干扰任务调度）
    xTaskCreate(camera_ai_task, "camera_ai_task", 16384, NULL, 7, NULL);
    xTaskCreate(inference_task, "inference_task", 8192, NULL, 4, NULL);
    xTaskCreate(storage_task, "storage_task", 8192, NULL, 4, NULL);
    // ⭐ 任务创建完毕后才启用 UART（UART ISR 不再抢占）
    uart_handler_0_init();
    uart_handler_1_init();

    // ⚠️ 延迟3秒确保SD卡DMA、PSRAM完全释放后再启动WiFi
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Starting web server (delayed init)...");
    web_server_init();

    // WiFi 就绪后再初始化 L610（避免L610 AT超时阻塞Web预览）
    ret = l610_manager_init();
    if (ret == ESP_OK) { ret = l610_manager_start(); if (ret == ESP_OK) ESP_LOGI(TAG, "L610 OK"); else ESP_LOGW(TAG, "L610 start fail"); }
    else ESP_LOGW(TAG, "L610 init fail");

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}