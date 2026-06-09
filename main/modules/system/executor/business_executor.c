#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "business_executor.h"
#include "cJSON.h"
#include "main.h"
#include "modules/ai/ai_module.h"
#include "modules/system/storage/storage_module.h"
#include "modules/system/verify/tag_id_validator.h"
#include "modules/system/verify/verify_handler.h"

static const char *TAG = "business_executor";

// ========== 内部状态机 ==========
// be_state_t 已移至 business_executor.h 公开

static be_response_cb_t g_be_cb = NULL;
static be_channel_t g_be_channel = BE_CHANNEL_UART1_JSON;
be_state_t g_be_state = BE_STATE_IDLE;
be_cmd_t g_be_task = BE_CMD_UNKNOWN;

// 任务上下文
char g_be_tag_id[TAG_ID_STR_LEN] = {0};
char g_be_item_name[128] = {0};
char g_be_storage_area = 'A';
uint32_t g_be_quantity = 0;
uint32_t g_be_remove_qty = 0;
static int g_be_total_views = 0;
static int g_be_captured_views = 0;
static bool g_be_is_verify_mode = false;

static void (*g_ws63_send_func)(const char *) = NULL;

// ========== 前向声明 ==========
static esp_err_t be_handle_register(be_channel_t channel, const char *tag_id, const char *params);
static esp_err_t be_handle_inventory(be_channel_t channel, const char *tag_id, const char *params);
static esp_err_t be_handle_outbound(be_channel_t channel, const char *tag_id, const char *params);
static esp_err_t be_handle_capture(be_channel_t channel, int view_index);
static esp_err_t be_handle_delete(be_channel_t channel, const char *tag_id, const char *params);
static esp_err_t be_handle_cancel(be_channel_t channel);
static esp_err_t be_handle_list_assets(be_channel_t channel, const char *params);
static esp_err_t be_handle_list_assets_page(be_channel_t channel, const char *params);
static esp_err_t be_handle_get_asset(be_channel_t channel, const char *tag_id, const char *params);
static esp_err_t be_handle_sys_info(be_channel_t channel);
static esp_err_t be_handle_ping(be_channel_t channel);
static void be_reset_state(void);

const char *be_get_state_string(void)
{
    switch (g_be_state) {
        case BE_STATE_IDLE:           return "idle";
        case BE_STATE_HARDWARE_INIT:  return "initializing";
        case BE_STATE_WAITING_CAPTURE:return "waiting_capture";
        case BE_STATE_CAPTURING:      return "capturing";
        case BE_STATE_FINALIZING:     return "finalizing";
        default:                      return "unknown";
    }
}

// ========== 取消命令 ==========
esp_err_t be_cancel(be_channel_t channel)
{
    if (g_be_state == BE_STATE_IDLE) {
        be_error_info_t err = { .error_code = -1, .error_msg = "No task to cancel" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_STATE;
    }
    be_cmd_t cancelled_task = g_be_task;
    extern bool g_camera_power_on;
    if (g_camera_power_on) {
        extern void led_camera_off(void);
        led_camera_off();
        g_camera_power_on = false;
        extern QueueHandle_t xSystemQueue;
        system_msg_t deinit_msg = { .cmd = CMD_DEINIT_CAMERA };
        xQueueSend(xSystemQueue, &deinit_msg, pdMS_TO_TICKS(500));
    }
    extern QueueHandle_t xInferenceQueue;
    inference_job_t discard;
    while (xQueueReceive(xInferenceQueue, &discard, 0) == pdTRUE) {}
    extern int g_views_enqueued;
    extern int g_views_processed;
    g_views_enqueued = 0;
    g_views_processed = 0;
    be_reset_state();
    be_task_done_t done = {0};
    done.task = cancelled_task;
    strncpy(done.tag_id, g_be_tag_id, sizeof(done.tag_id) - 1);
    strncpy(done.item_name, g_be_item_name, sizeof(done.item_name) - 1);
    done.result = (const char*)"cancelled";
    g_be_cb(channel, BE_EVT_TASK_DONE, &done);
    ESP_LOGI(TAG, "Task cancelled: %d", cancelled_task);
    return ESP_OK;
}

// ========== be_execute 核心分发 ==========
esp_err_t be_execute(be_channel_t channel, be_cmd_t cmd,
                     const char *tag_id, const char *params)
{
    g_be_channel = channel;

    // ⭐ 状态检查：IDLE 状态下接受所有命令；
    // WAITING_CAPTURE / CAPTURING 状态下接受 capture 命令；
    // 所有 BUSY 状态均可接受 cancel
    if (g_be_state != BE_STATE_IDLE) {
        bool is_allowed = false;
        if (cmd == BE_CMD_CANCEL) is_allowed = true;
        if (g_be_state == BE_STATE_WAITING_CAPTURE || g_be_state == BE_STATE_CAPTURING) {
            if (cmd == BE_CMD_CAPTURE_FRONT || cmd == BE_CMD_CAPTURE_SIDE ||
                cmd == BE_CMD_CAPTURE_TOP) {
                is_allowed = true;
            }
        }
        if (!is_allowed) {
            be_error_info_t err = { .error_code = -1, .error_msg = "Another task is in progress" };
            g_be_cb(channel, BE_EVT_ERROR, &err);
            return ESP_ERR_INVALID_STATE;
        }
    }

    switch (cmd) {
        case BE_CMD_REGISTER:         return be_handle_register(channel, tag_id, params);
        case BE_CMD_INVENTORY:        return be_handle_inventory(channel, tag_id, params);
        case BE_CMD_OUTBOUND:         return be_handle_outbound(channel, tag_id, params);
        case BE_CMD_CAPTURE_FRONT:    return be_handle_capture(channel, 0);
        case BE_CMD_CAPTURE_SIDE:     return be_handle_capture(channel, 1);
        case BE_CMD_CAPTURE_TOP:      return be_handle_capture(channel, 2);
        case BE_CMD_DELETE:           return be_handle_delete(channel, tag_id, params);
        case BE_CMD_CANCEL:           return be_handle_cancel(channel);
        case BE_CMD_LIST_ASSETS:      return be_handle_list_assets(channel, params);
        case BE_CMD_LIST_ASSETS_PAGE: return be_handle_list_assets_page(channel, params);
        case BE_CMD_GET_ASSET:        return be_handle_get_asset(channel, tag_id, params);
        case BE_CMD_SYS_INFO:         return be_handle_sys_info(channel);
        case BE_CMD_PING:             return be_handle_ping(channel);
        default:
            be_error_info_t err = { .error_code = -1, .error_msg = "Unknown command" };
            g_be_cb(channel, BE_EVT_ERROR, &err);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

// ========== 注册处理 ==========
static esp_err_t be_handle_register(be_channel_t channel, const char *tag_id, const char *params)
{
    if (tag_id == NULL || strlen(tag_id) == 0) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Missing tag_id" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Tag ID 校验
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    normalized[TAG_ID_STR_LEN - 1] = '\0';
    tag_id_validator_normalize(normalized);
    if (!tag_id_validator_validate(normalized)) {
        be_error_info_t err = { .error_code = -1, .error_msg = tag_id_validator_get_error_string() };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }

    // 2. 检查资产是否已存在（堆分配避免栈溢出，asset_record_t ~15KB）
    asset_record_t *existing = (asset_record_t *)malloc(sizeof(asset_record_t));
    if (!existing) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Out of memory" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NO_MEM;
    }
    bool has_existing = (asset_load(normalized, existing) == ESP_OK && existing->is_valid);
    if (has_existing) {
        // TODO: 验证式更新
    }

    // 3. 保存任务上下文
    strncpy(g_be_tag_id, normalized, sizeof(g_be_tag_id) - 1);
    g_be_tag_id[sizeof(g_be_tag_id) - 1] = '\0';

    // ⭐ 从 params JSON 提取注册参数（修复：之前 TODO 未实现）
    if (params != NULL && params[0] == '{') {
        cJSON *json = cJSON_Parse(params);
        if (json) {
            cJSON *item = cJSON_GetObjectItem(json, "item_name");
            cJSON *area = cJSON_GetObjectItem(json, "storage_area");
            cJSON *qty  = cJSON_GetObjectItem(json, "quantity");
            if (item && cJSON_IsString(item))
                strncpy(g_be_item_name, item->valuestring, sizeof(g_be_item_name) - 1);
            if (area && cJSON_IsString(area) && strlen(area->valuestring) > 0)
                g_be_storage_area = toupper((unsigned char)area->valuestring[0]);
            if (qty && cJSON_IsNumber(qty))
                g_be_quantity = (uint32_t)qty->valueint;
            cJSON_Delete(json);
        }
    } else {
        extern char g_reg_item_name[];
        extern char g_reg_storage_area;
        extern uint32_t g_reg_quantity;
        strncpy(g_be_item_name, g_reg_item_name, sizeof(g_be_item_name) - 1);
        g_be_storage_area = g_reg_storage_area;
        g_be_quantity = g_reg_quantity;
    }

    g_be_task = BE_CMD_REGISTER;
    g_be_total_views = 3;
    g_be_state = BE_STATE_HARDWARE_INIT;

    // 4. 硬件初始化（AI 模型已在 app_main 中开机加载）
    extern bool g_is_inventory_mode;
    extern bool g_is_outbound_mode;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    extern int g_total_views;
    g_total_views = 3;
    extern char g_current_tag_id[];
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);

    // 4b. 先发送初始化存储指令（与 cmd_handler 一致，确保 SD 卡就绪）
    extern QueueHandle_t xStorageQueue;
    system_msg_t init_storage_msg = {0};
    init_storage_msg.cmd = CMD_INIT_STORAGE;
    xQueueSend(xStorageQueue, &init_storage_msg, portMAX_DELAY);

    // 4c. 通过 xSystemQueue 异步初始化摄像头（与 be_handle_inventory 一致）
    // 在 camera_ai_task 上下文中安全执行 i2c_driver_install，避免 UART ISR 冲突
    extern QueueHandle_t xSystemQueue;
    system_msg_t cam_init_msg = {0};
    cam_init_msg.cmd = CMD_INIT_CAMERA;
    snprintf(cam_init_msg.tag_id, sizeof(cam_init_msg.tag_id), "%s", normalized);
    if (!xQueueSend(xSystemQueue, &cam_init_msg, pdMS_TO_TICKS(500))) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Camera init queue full" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        be_reset_state();
        return ESP_ERR_TIMEOUT;
    }
    // g_camera_ready / g_camera_power_on 将在 camera_ai_task 中由
    // CMD_INIT_CAMERA 处理函数在 camera_module_init() 成功后设置

    // 5. 通知上层：硬件就绪，等待拍摄
    be_hardware_ready_t ready = {0};
    strncpy(ready.tag_id, normalized, sizeof(ready.tag_id) - 1);
    ready.total_views = 3;
    strncpy(ready.item_name, g_be_item_name, sizeof(ready.item_name) - 1);
    ready.storage_area = g_be_storage_area;
    ready.quantity = g_be_quantity;
    g_be_cb(channel, BE_EVT_HARDWARE_READY, &ready);

    free(existing);
    g_be_state = BE_STATE_WAITING_CAPTURE;
    ESP_LOGI(TAG, "Register started: tag_id=%s, item=%s, qty=%lu",
             normalized, g_be_item_name, (unsigned long)g_be_quantity);
    return ESP_OK;
}

// ========== 盘点处理 ==========
static esp_err_t be_handle_inventory(be_channel_t channel, const char *tag_id, const char *params)
{
    if (tag_id == NULL || strlen(tag_id) == 0) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Missing tag_id" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    normalized[TAG_ID_STR_LEN - 1] = '\0';
    tag_id_validator_normalize(normalized);
    if (!tag_id_validator_validate(normalized)) {
        be_error_info_t err = { .error_code = -1, .error_msg = tag_id_validator_get_error_string() };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    // 堆分配避免栈溢出，asset_record_t ~15KB
    asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
    if (!record) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Out of memory" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = asset_load(normalized, record);
    if (ret != ESP_OK) {
        free(record);
        be_error_info_t err = { .error_code = -1, .error_msg = "Asset not found" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(g_stored_front_feature, record->front_feature, FEATURE_VEC_SIZE * sizeof(float));
    memcpy(g_stored_side_feature, record->side_feature, FEATURE_VEC_SIZE * sizeof(float));
    memcpy(g_stored_top_feature, record->top_feature, FEATURE_VEC_SIZE * sizeof(float));
    strncpy(g_be_tag_id, normalized, sizeof(g_be_tag_id) - 1);
    strncpy(g_be_item_name, record->item_name, sizeof(g_be_item_name) - 1);
    g_be_storage_area = record->storage_area;
    g_be_quantity = record->quantity;
    free(record);
    g_be_task = BE_CMD_INVENTORY;
    g_be_total_views = 3;
    g_be_state = BE_STATE_HARDWARE_INIT;
    extern bool g_is_inventory_mode;
    g_is_inventory_mode = true;
    extern int g_total_views;
    g_total_views = 3;
    extern char g_current_tag_id[];
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);

    // 先发送初始化存储指令（与 cmd_handler 一致）
    extern QueueHandle_t xStorageQueue;
    system_msg_t init_storage_msg = {0};
    init_storage_msg.cmd = CMD_INIT_STORAGE;
    if (!xQueueSend(xStorageQueue, &init_storage_msg, pdMS_TO_TICKS(200))) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Storage queue full" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        be_reset_state();
        return ESP_ERR_TIMEOUT;
    }

    system_msg_t init_msg = {0};
    init_msg.cmd = CMD_INIT_CAMERA;
    snprintf(init_msg.tag_id, sizeof(init_msg.tag_id), "%s", normalized);
    if (!xQueueSend(xSystemQueue, &init_msg, pdMS_TO_TICKS(200))) {
        be_error_info_t err = { .error_code = -1, .error_msg = "System queue full" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        be_reset_state();
        return ESP_ERR_TIMEOUT;
    }
    be_hardware_ready_t ready = {0};
    strncpy(ready.tag_id, normalized, sizeof(ready.tag_id) - 1);
    ready.total_views = 3;
    strncpy(ready.item_name, g_be_item_name, sizeof(ready.item_name) - 1);
    ready.storage_area = g_be_storage_area;
    ready.quantity = g_be_quantity;
    g_be_cb(channel, BE_EVT_HARDWARE_READY, &ready);
    g_be_state = BE_STATE_WAITING_CAPTURE;
    ESP_LOGI(TAG, "Inventory started: tag_id=%s, item=%s", normalized, g_be_item_name);
    return ESP_OK;
}

// ========== 出库处理 ==========
static esp_err_t be_handle_outbound(be_channel_t channel, const char *tag_id, const char *params)
{
    if (tag_id == NULL || strlen(tag_id) == 0) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Missing tag_id" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    normalized[TAG_ID_STR_LEN - 1] = '\0';
    tag_id_validator_normalize(normalized);
    if (!tag_id_validator_validate(normalized)) {
        be_error_info_t err = { .error_code = -1, .error_msg = tag_id_validator_get_error_string() };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }

    // 加载现有资产（堆分配，asset_record_t ~15KB）
    asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
    if (!record) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Out of memory" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NO_MEM;
    }
    if (asset_load(normalized, record) != ESP_OK || !record->is_valid) {
        free(record);
        be_error_info_t err = { .error_code = -1, .error_msg = "Asset not found" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NOT_FOUND;
    }

    // 提取 remove_qty
    if (params != NULL && params[0] == '{') {
        cJSON *json = cJSON_Parse(params);
        if (json) {
            cJSON *j_qty = cJSON_GetObjectItem(json, "remove_qty");
            if (j_qty && cJSON_IsNumber(j_qty) && j_qty->valueint > 0)
                g_be_remove_qty = (uint32_t)j_qty->valueint;
            cJSON_Delete(json);
        }
    }
    if (g_be_remove_qty == 0) g_be_remove_qty = 1;

    // 保存上下文
    strncpy(g_be_tag_id, normalized, sizeof(g_be_tag_id) - 1);
    strncpy(g_be_item_name, record->item_name, sizeof(g_be_item_name) - 1);
    g_be_storage_area = record->storage_area;
    g_be_quantity = record->quantity;
    free(record);

    g_be_task = BE_CMD_OUTBOUND;
    g_be_total_views = 1;  // 仅正视图
    // 摄像头不在此处初始化——等 capture 命令到达时按需初始化

    extern bool g_is_outbound_mode;
    extern bool g_is_inventory_mode;
    g_is_outbound_mode = false;
    g_is_inventory_mode = false;

    extern int g_total_views;
    g_total_views = 1;
    extern char g_current_tag_id[];
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);

    // 初始化存储（SD 卡）
    extern QueueHandle_t xStorageQueue;
    system_msg_t init_storage_msg = {0};
    init_storage_msg.cmd = CMD_INIT_STORAGE;
    xQueueSend(xStorageQueue, &init_storage_msg, portMAX_DELAY);

    // 发送 asset_info（不初始化摄像头，等待 WS63 确认后发 capture）
    uint32_t remaining = (g_be_remove_qty >= g_be_quantity) ? 0 :
                         (g_be_quantity - g_be_remove_qty);
    be_asset_info_t info = {0};
    strncpy(info.tag_id, normalized, sizeof(info.tag_id) - 1);
    strncpy(info.item_name, g_be_item_name, sizeof(info.item_name) - 1);
    info.storage_area = g_be_storage_area;
    info.quantity = g_be_quantity;
    info.remove_qty = g_be_remove_qty;
    info.remaining_qty = remaining;
    g_be_cb(channel, BE_EVT_ASSET_INFO, &info);

    g_be_state = BE_STATE_WAITING_CAPTURE;
    ESP_LOGI(TAG, "Outbound started: tag_id=%s, item=%s, remove_qty=%lu, remaining=%lu",
             normalized, g_be_item_name, (unsigned long)g_be_remove_qty, (unsigned long)remaining);
    return ESP_OK;
}

// ========== 拍摄处理 ==========
static esp_err_t be_handle_capture(be_channel_t channel, int view_index)
{
    if (g_be_state != BE_STATE_WAITING_CAPTURE && g_be_state != BE_STATE_CAPTURING) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Not in capture state" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_STATE;
    }

    // 出库按需初始化：capture 到达时才初始化摄像头
    extern bool g_camera_ready;
    if (!g_camera_ready && g_be_task == BE_CMD_OUTBOUND) {
        extern QueueHandle_t xSystemQueue;
        system_msg_t cam_init_msg = {0};
        cam_init_msg.cmd = CMD_INIT_CAMERA;
        snprintf(cam_init_msg.tag_id, sizeof(cam_init_msg.tag_id), "%s", g_be_tag_id);
        if (!xQueueSend(xSystemQueue, &cam_init_msg, pdMS_TO_TICKS(500))) {
            be_error_info_t err = { .error_code = -1, .error_msg = "Camera init queue full" };
            g_be_cb(channel, BE_EVT_ERROR, &err);
            return ESP_ERR_TIMEOUT;
        }
        be_hardware_ready_t ready = {0};
        strncpy(ready.tag_id, g_be_tag_id, sizeof(ready.tag_id) - 1);
        ready.total_views = g_be_total_views;
        strncpy(ready.item_name, g_be_item_name, sizeof(ready.item_name) - 1);
        ready.storage_area = g_be_storage_area;
        ready.quantity = g_be_quantity;
        g_be_cb(channel, BE_EVT_HARDWARE_READY, &ready);
    }

    g_be_state = BE_STATE_CAPTURING;
    system_cmd_t view_cmd;
    if (view_index == 0) view_cmd = CMD_CAPTURE_FRONT;
    else if (view_index == 1) view_cmd = CMD_CAPTURE_SIDE;
    else view_cmd = CMD_CAPTURE_TOP;
    system_msg_t msg = {0};
    msg.cmd = view_cmd;
    snprintf(msg.tag_id, sizeof(msg.tag_id), "%s", g_be_tag_id);
    if (!xQueueSend(xSystemQueue, &msg, pdMS_TO_TICKS(200))) {
        be_error_info_t err = { .error_code = -1, .error_msg = "System queue full, capture dropped" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        ESP_LOGW(TAG, "Capture dropped: queue full, view=%d", view_index);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Capture dispatched: view=%d, tag_id=%s", view_index, g_be_tag_id);
    return ESP_OK;
}

// ========== 删除处理 ==========
static esp_err_t be_handle_delete(be_channel_t channel, const char *tag_id, const char *params)
{
    if (tag_id == NULL || strlen(tag_id) == 0) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Missing tag_id" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    normalized[TAG_ID_STR_LEN - 1] = '\0';
    tag_id_validator_normalize(normalized);
    if (!tag_id_validator_validate(normalized)) {
        be_error_info_t err = { .error_code = -1, .error_msg = tag_id_validator_get_error_string() };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = asset_delete(normalized);
    be_task_done_t done = {0};
    done.task = BE_CMD_DELETE;
    strncpy(done.tag_id, normalized, sizeof(done.tag_id) - 1);
    done.result = (ret == ESP_OK) ? "success" : "failed";
    g_be_cb(channel, BE_EVT_TASK_DONE, &done);
    return ret;
}

static esp_err_t be_handle_cancel(be_channel_t channel) { return be_cancel(channel); }

static esp_err_t be_handle_list_assets(be_channel_t channel, const char *params)
{
    // 委托给分页版本（默认 page=1, page_size=50）
    return be_handle_list_assets_page(channel, params ? params : "{\"page\":1,\"page_size\":50}");
}

static esp_err_t be_handle_list_assets_page(be_channel_t channel, const char *params)
{
    int page = 1;
    int page_size = 6;
    if (params != NULL && params[0] == '{') {
        cJSON *json = cJSON_Parse(params);
        if (json) {
            cJSON *j_page = cJSON_GetObjectItem(json, "page");
            cJSON *j_size = cJSON_GetObjectItem(json, "page_size");
            if (j_page && cJSON_IsNumber(j_page)) page = j_page->valueint;
            if (j_size && cJSON_IsNumber(j_size)) page_size = j_size->valueint;
            cJSON_Delete(json);
        }
    }
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 6;
    if (page_size > 50) page_size = 50;

    // ⭐ 扫描 assets 目录：先收集所有 tag_id（支持子目录+平铺兼容）
    #define MAX_ASSETS_SCAN 200
    char tag_ids[MAX_ASSETS_SCAN][TAG_ID_STR_LEN];
    int total_count = 0;

    DIR *dir = opendir("/sdcard/assets");
    if (!dir) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Storage not available" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        // 尝试作为子目录打开（Tag ID 格式的子文件夹）
        char sub_path[320];
        snprintf(sub_path, sizeof(sub_path), "/sdcard/assets/%.255s", entry->d_name);
        DIR *sub = opendir(sub_path);
        if (sub) {
            struct dirent *se;
            while ((se = readdir(sub)) != NULL && total_count < MAX_ASSETS_SCAN) {
                if (strstr(se->d_name, ".dat")) {
                    strncpy(tag_ids[total_count], se->d_name, TAG_ID_STR_LEN - 1);
                    tag_ids[total_count][TAG_ID_STR_LEN - 1] = '\0';
                    char *dot = strchr(tag_ids[total_count], '.');
                    if (dot) *dot = '\0';
                    total_count++;
                }
            }
            closedir(sub);
            continue;
        }

        // 旧格式平铺文件（向后兼容）
        if (strstr(entry->d_name, ".dat") && total_count < MAX_ASSETS_SCAN) {
            strncpy(tag_ids[total_count], entry->d_name, TAG_ID_STR_LEN - 1);
            tag_ids[total_count][TAG_ID_STR_LEN - 1] = '\0';
            char *dot = strchr(tag_ids[total_count], '.');
            if (dot) *dot = '\0';
            total_count++;
        }
    }
    closedir(dir);

    int total_pages = (total_count + page_size - 1) / page_size;
    if (total_pages < 1) total_pages = 1;

    // 计算偏移
    int skip = (page - 1) * page_size;
    int item_idx = 0;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "asset_list_page");
    cJSON_AddNumberToObject(resp, "page", page);
    cJSON_AddNumberToObject(resp, "total_pages", total_pages);
    cJSON_AddNumberToObject(resp, "total_count", total_count);
    cJSON *assets_arr = cJSON_AddArrayToObject(resp, "assets");

    for (int i = skip; i < total_count && item_idx < page_size; i++) {
        asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
        if (!record) continue;
        if (asset_load(tag_ids[i], record) == ESP_OK && record->is_valid) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "tag_id", record->tag_id);
            cJSON_AddStringToObject(item, "item_name", record->item_name);
            char area_str[2] = {record->storage_area, '\0'};
            cJSON_AddStringToObject(item, "storage_area", area_str);
            cJSON_AddNumberToObject(item, "quantity", record->quantity);
            cJSON_AddItemToArray(assets_arr, item);
            item_idx++;
        }
        free(record);
    }

    char *js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    be_asset_list_t list_result = {0};
    list_result.total_count = total_count;
    strncpy(list_result.json_payload, js ? js : "{}", sizeof(list_result.json_payload) - 1);
    if (js) free(js);

    g_be_cb(channel, BE_EVT_ASSET_LIST_RESULT, &list_result);
    return ESP_OK;
}

static esp_err_t be_handle_get_asset(be_channel_t channel, const char *tag_id, const char *params)
{
    if (tag_id == NULL || strlen(tag_id) == 0) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Missing tag_id" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    normalized[TAG_ID_STR_LEN - 1] = '\0';
    tag_id_validator_normalize(normalized);

    asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
    if (!record) {
        be_error_info_t err = { .error_code = -1, .error_msg = "Out of memory" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_NO_MEM;
    }

    be_asset_detail_t detail = {0};
    strncpy(detail.tag_id, normalized, sizeof(detail.tag_id) - 1);

    if (asset_load(normalized, record) == ESP_OK && record->is_valid) {
        detail.found = true;
        strncpy(detail.item_name, record->item_name, sizeof(detail.item_name) - 1);
        detail.storage_area = record->storage_area;
        detail.quantity = record->quantity;
    } else {
        detail.found = false;
    }
    free(record);

    g_be_cb(channel, BE_EVT_ASSET_DETAIL, &detail);
    return ESP_OK;
}

static esp_err_t be_handle_sys_info(be_channel_t channel)
{
    be_sys_info_t info = {0};
    info.free_heap = esp_get_free_heap_size();
    info.camera_ready = g_camera_ready;
    info.storage_ready = g_storage_ready;
    info.state_str = be_get_state_string();

    uint64_t total = 0, used = 0, free_bytes = 0;
    if (asset_get_storage_info(&total, &used, &free_bytes) == ESP_OK) {
        info.storage_total_mb = (uint32_t)(total / (1024 * 1024));
        info.storage_free_mb = (uint32_t)(free_bytes / (1024 * 1024));
    }

    g_be_cb(channel, BE_EVT_SYS_INFO_RESULT, &info);
    return ESP_OK;
}

static esp_err_t be_handle_ping(be_channel_t channel)
{
    be_pong_t pong = {0};
    pong.camera_ready = g_camera_ready;
    pong.storage_ready = g_storage_ready;
    pong.free_heap = esp_get_free_heap_size();
    pong.state_str = be_get_state_string();

    g_be_cb(channel, BE_EVT_PONG_RESULT, &pong);
    return ESP_OK;
}

// ========== 视图回调 ==========
void be_on_view_captured(int view_index)
{
    ESP_LOGI(TAG, "View captured g_be_channel=%d", (int)g_be_channel);
    g_be_captured_views++;
    be_capture_progress_t prog = {0};
    strncpy(prog.tag_id, g_be_tag_id, sizeof(prog.tag_id) - 1);
    prog.view_index = view_index;
    prog.total_steps = g_be_total_views;
    g_be_cb(g_be_channel, BE_EVT_CAPTURE_PROGRESS, &prog);
    ESP_LOGI(TAG, "View captured: %d/%d (view_idx=%d)", g_be_captured_views, g_be_total_views, view_index);
}

bool be_on_all_views_done(void)
{
    ESP_LOGI(TAG, "All views done g_be_channel=%d", (int)g_be_channel);
    g_be_state = BE_STATE_FINALIZING;
    bool handled = (g_be_task != BE_CMD_UNKNOWN);
    switch (g_be_task) {
        case BE_CMD_REGISTER: {
            asset_record_t *record = (asset_record_t *)malloc(sizeof(asset_record_t));
            if (!record) {
                be_task_done_t done = {0};
                done.task = BE_CMD_REGISTER;
                done.result = "failed (OOM)";
                g_be_cb(g_be_channel, BE_EVT_TASK_DONE, &done);
                break;
            }
            memset(record, 0, sizeof(*record));
            strncpy(record->tag_id, g_be_tag_id, sizeof(record->tag_id) - 1);
            strncpy(record->item_name, g_be_item_name, sizeof(record->item_name) - 1);
            record->storage_area = g_be_storage_area;
            record->quantity = g_be_quantity;
            memcpy(record->front_feature, g_front_feature, sizeof(record->front_feature));
            memcpy(record->side_feature, g_side_feature, sizeof(record->side_feature));
            memcpy(record->top_feature, g_top_feature, sizeof(record->top_feature));
            record->is_valid = true;
            bool is_overwrite = false;
            esp_err_t ret = asset_save(record, &is_overwrite);
            free(record);
            be_task_done_t done = {0};
            done.task = BE_CMD_REGISTER;
            strncpy(done.tag_id, g_be_tag_id, sizeof(done.tag_id) - 1);
            strncpy(done.item_name, g_be_item_name, sizeof(done.item_name) - 1);
            done.storage_area = g_be_storage_area;
            done.quantity = g_be_quantity;
            done.is_overwrite = is_overwrite;
            done.result = (ret == ESP_OK) ? "success" : "failed";
            g_be_cb(g_be_channel, BE_EVT_TASK_DONE, &done);
            break;
        }
        case BE_CMD_INVENTORY: {
            similarity_result_t front_result = {0}, side_result = {0}, top_result = {0};
            extern bool ai_module_match_features(const float *, const float *, int, asset_class_t, similarity_result_t *);
            ai_module_match_features(g_front_feature, g_stored_front_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &front_result);
            ai_module_match_features(g_side_feature, g_stored_side_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &side_result);
            ai_module_match_features(g_top_feature, g_stored_top_feature, FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &top_result);
            float weighted_conf = front_result.confidence * 0.5f + side_result.confidence * 0.3f + top_result.confidence * 0.2f;
            bool is_match = (weighted_conf >= front_result.match_threshold);
            be_task_done_t done = {0};
            done.task = BE_CMD_INVENTORY;
            strncpy(done.tag_id, g_be_tag_id, sizeof(done.tag_id) - 1);
            strncpy(done.item_name, g_be_item_name, sizeof(done.item_name) - 1);
            done.storage_area = g_be_storage_area;
            done.quantity = g_be_quantity;
            done.is_match = is_match;
            done.confidence = weighted_conf;
            done.threshold = front_result.match_threshold;
            done.result = "success";
            g_be_cb(g_be_channel, BE_EVT_TASK_DONE, &done);
            break;
        }
        case BE_CMD_OUTBOUND: {
            be_task_done_t done = {0};
            done.task = BE_CMD_OUTBOUND;
            strncpy(done.tag_id, g_be_tag_id, sizeof(done.tag_id) - 1);
            strncpy(done.item_name, g_be_item_name, sizeof(done.item_name) - 1);
            done.previous_qty = g_be_quantity;
            done.remove_qty = g_be_remove_qty;

            // 加载存储的特征向量进行比对
            asset_record_t *stored = (asset_record_t *)malloc(sizeof(asset_record_t));
            if (stored && asset_load(g_be_tag_id, stored) == ESP_OK && stored->is_valid) {
                similarity_result_t result = {0};
                ai_module_match_features(g_front_feature, stored->front_feature,
                    FEATURE_VEC_SIZE, ASSET_CLASS_UNKNOWN, &result);
                done.is_match = (result.confidence >= result.match_threshold);
                done.confidence = result.confidence;
                done.threshold = result.match_threshold;

                if (done.is_match) {
                    // 匹配成功：扣减数量
                    uint32_t new_qty = stored->quantity;
                    if (g_be_remove_qty >= new_qty) new_qty = 0;
                    else new_qty -= g_be_remove_qty;
                    done.quantity = new_qty;
                    bool ow = false;
                    if (new_qty == 0) {
                        asset_delete(g_be_tag_id);
                    } else {
                        stored->quantity = new_qty;
                        asset_save(stored, &ow);
                    }
                    done.result = "success";
                } else {
                    done.quantity = stored->quantity;
                    done.result = "failed (mismatch)";
                }
                free(stored);
            } else {
                if (stored) free(stored);
                done.result = "failed (load error)";
            }
            g_be_cb(g_be_channel, BE_EVT_TASK_DONE, &done);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown task type in be_on_all_views_done: %d", g_be_task);
            handled = false;
            break;
    }
    be_reset_state();
    return handled;
}

static void be_reset_state(void)
{
    g_be_state = BE_STATE_IDLE;
    g_be_task = BE_CMD_UNKNOWN;
    memset(g_be_tag_id, 0, sizeof(g_be_tag_id));
    memset(g_be_item_name, 0, sizeof(g_be_item_name));
    g_be_storage_area = 'A';
    g_be_quantity = 0;
    g_be_remove_qty = 0;
    g_be_total_views = 0;
    g_be_captured_views = 0;
    g_be_is_verify_mode = false;

    // 复位 camera 状态到主菜单（与 system_shutdown_camera() 一致）
    g_camera_state = CAM_STATE_WAITING_TAG_ID;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    g_inventory_state = INVENTORY_IDLE;
}

void be_register_ws63_send_func(void (*func)(const char *))
{
    g_ws63_send_func = func;
    ESP_LOGI(TAG, "WS63 send function registered");
}

void be_init(be_response_cb_t resp_cb)
{
    g_be_cb = resp_cb;
    g_be_state = BE_STATE_IDLE;
    ESP_LOGI(TAG, "Business executor initialized");
}