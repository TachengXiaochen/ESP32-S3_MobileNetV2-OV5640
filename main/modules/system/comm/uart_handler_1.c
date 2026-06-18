/*
 * uart_handler_1.c — UART1 WS63 JSON 协议处理器
 *
 * 职责：
 * - 初始化 UART1 硬件（GPIO47 TX, GPIO21 RX, 115200）
 * - 接收 WS63 JSON 命令 → 映射到 business_executor
 * - 将 business_executor 事件格式化为 WS63 JSON 协议输出
 * - L610 4G 命令直接处理（不经过 business_executor）
 *
 * 参考：原 protocol_handler.c（JSON 解析 + L610 4G 命令）
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "uart_handler_1.h"
#include "main.h"
#include "modules/4g/l610_manager.h"
#include "modules/4g/l610_mqtt.h"
#include "modules/4g/l610_driver.h"

static const char *TAG = "uart1_handler";

// 外部声明（集中管理，避免散落在函数体内）
// 注：g_l610_client_id 已通过 main.h 宏映射到 g_ctx.l610_client_id
extern void l610_manager_register_send_func(void (*)(const char *));

#define WS63_UART_NUM    UART_NUM_1
#define WS63_UART_BAUD   115200

static char g_l610_last_host[128] = "unknown";
static uint16_t g_l610_last_port = 1883;

// ===== UART1 初始化 =====
static esp_err_t uart1_uart_init(void)
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
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(WS63_UART_NUM, WS63_UART_TX_PIN, WS63_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(WS63_UART_NUM, WS63_UART_BUF_SIZE, 0, 0, NULL, 0);
    return ret;
}

// ===== 发送 JSON（通过 UART1）=====
void uart_handler_1_send_json(const char *json_str)
{
    if (json_str == NULL) return;

    size_t len = strlen(json_str);
    char *msg = malloc(len + 2);
    if (!msg) return;

    snprintf(msg, len + 2, "%s\n", json_str);
    int written = uart_write_bytes(WS63_UART_NUM, msg, len + 1);
    free(msg);

    if (written < 0) {
        ESP_LOGE(TAG, "TX FAILED err=%d: %s", written, json_str);
    } else {
        ESP_LOGI(TAG, "Sent to WS63 (%d bytes): %s", written, json_str);
    }
}

// ===== L610 4G 命令处理（保留原实现）=====
static void uart1_handle_l610_connect(cJSON *json_obj)
{
    cJSON *host_item = cJSON_GetObjectItem(json_obj, "host");
    cJSON *port_item = cJSON_GetObjectItem(json_obj, "port");

    if (!host_item || !cJSON_IsString(host_item)) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"MISSING_FIELD\",\"msg\":\"Missing host\"}");
        return;
    }

    uint16_t port = L610_MQTT_DEFAULT_PORT;
    if (port_item && cJSON_IsNumber(port_item)) port = (uint16_t)port_item->valueint;

    strncpy(g_l610_last_host, host_item->valuestring, sizeof(g_l610_last_host) - 1);
    g_l610_last_port = port;

    cJSON *clean_session_item = cJSON_GetObjectItem(json_obj, "clean_session");
    cJSON *keepalive_item = cJSON_GetObjectItem(json_obj, "keepalive");
    uint8_t clean_session = 1;
    uint16_t keepalive = 60;
    if (clean_session_item && cJSON_IsNumber(clean_session_item)) clean_session = (uint8_t)clean_session_item->valueint;
    if (keepalive_item && cJSON_IsNumber(keepalive_item)) keepalive = (uint16_t)keepalive_item->valueint;

    if (strlen(g_l610_client_id) > 0) {
        l610_mqtt_set_user(g_l610_client_id, NULL, NULL);
    }

    esp_err_t ret = l610_mqtt_connect(host_item->valuestring, port, clean_session, keepalive, 15);

    char resp_buf[512];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(resp, "type", "mqtt_connected");
            cJSON_AddStringToObject(resp, "state", "connected");
            cJSON_AddStringToObject(resp, "host", host_item->valuestring);
            cJSON_AddNumberToObject(resp, "port", port);
        } else {
            cJSON_AddStringToObject(resp, "type", "mqtt_error");
            cJSON_AddStringToObject(resp, "code", "MQTT_CONNECT_FAILED");
            cJSON_AddStringToObject(resp, "msg", "MQTT connection failed");
        }
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
}

static void uart1_handle_l610_disconnect(cJSON *json_obj)
{
    (void)json_obj;
    esp_err_t ret = l610_mqtt_disconnect(5);
    char resp_buf[256];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddStringToObject(resp, "type", "mqtt_connected");
        cJSON_AddStringToObject(resp, "state", "disconnected");
        cJSON_AddStringToObject(resp, "host", g_l610_last_host);
        cJSON_AddNumberToObject(resp, "port", g_l610_last_port);
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
    (void)ret;
}

static void uart1_handle_l610_publish(cJSON *json_obj)
{
    cJSON *topic_item = cJSON_GetObjectItem(json_obj, "topic");
    cJSON *payload_item = cJSON_GetObjectItem(json_obj, "payload");
    cJSON *qos_item = cJSON_GetObjectItem(json_obj, "qos");
    cJSON *retain_item = cJSON_GetObjectItem(json_obj, "retain");

    if (!topic_item || !cJSON_IsString(topic_item) || !payload_item || !cJSON_IsString(payload_item)) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"MISSING_FIELD\",\"msg\":\"Missing topic or payload\"}");
        return;
    }

    int qos = 1;
    if (qos_item && cJSON_IsNumber(qos_item)) { qos = qos_item->valueint; if (qos < 0 || qos > 2) qos = 1; }
    uint8_t retain = 0;
    if (retain_item && cJSON_IsNumber(retain_item)) retain = (uint8_t)(retain_item->valueint != 0);

    esp_err_t ret = l610_mqtt_publish(topic_item->valuestring, payload_item->valuestring,
                                       (uint8_t)qos, retain, 8);

    char resp_buf[512];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(resp, "type", "mqtt_publish_done");
            cJSON_AddStringToObject(resp, "result", "success");
            cJSON_AddStringToObject(resp, "topic", topic_item->valuestring);
        } else {
            cJSON_AddStringToObject(resp, "type", "l610_error");
            cJSON_AddStringToObject(resp, "code", "L610_MQTT_PUBLISH_FAIL");
            cJSON_AddStringToObject(resp, "msg", "MQTT publish failed");
        }
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
}

static void uart1_handle_l610_status(void)
{
    l610_status_t st;
    esp_err_t ret = l610_manager_get_status(&st);
    char resp_buf[256];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddStringToObject(resp, "type", "l610_status");
        const char *mqtt_str = "disconnected";
        if (st.mqtt_state == MQTT_STATE_CONNECTED) mqtt_str = "connected";
        else if (st.mqtt_state == MQTT_STATE_CONNECTING) mqtt_str = "connecting";
        else if (st.mqtt_state == MQTT_STATE_DISCONNECTING) mqtt_str = "disconnecting";
        else if (st.mqtt_state == MQTT_STATE_RECONNECTING) mqtt_str = "reconnecting";
        else if (st.mqtt_state == MQTT_STATE_ERROR) mqtt_str = "error";
        cJSON_AddStringToObject(resp, "mqtt_state", mqtt_str);
        cJSON_AddNumberToObject(resp, "signal_quality", st.signal_quality);
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
    (void)ret;
}

static void uart1_handle_l610_at(cJSON *json_obj)
{
    cJSON *at_item = cJSON_GetObjectItem(json_obj, "at");
    if (!at_item || !cJSON_IsString(at_item)) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"MISSING_FIELD\",\"msg\":\"Missing at\"}");
        return;
    }
    char response_buf[1024];
    esp_err_t ret = l610_at_send(at_item->valuestring, response_buf, sizeof(response_buf), L610_AT_DEFAULT_TIMEOUT);

    char resp_buf[1536];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(resp, "type", "l610_at_result");
            cJSON_AddStringToObject(resp, "cmd", at_item->valuestring);
            cJSON_AddStringToObject(resp, "result", "ok");
            size_t rlen = strlen(response_buf);
            while (rlen > 0 && (response_buf[rlen-1] == '\n' || response_buf[rlen-1] == '\r')) response_buf[--rlen] = '\0';
            cJSON_AddStringToObject(resp, "response", response_buf);
        } else {
            cJSON_AddStringToObject(resp, "type", "l610_error");
            cJSON_AddStringToObject(resp, "code", "L610_AT_TIMEOUT");
            cJSON_AddStringToObject(resp, "msg", "L610 not responding");
        }
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
}

static void uart1_handle_l610_mqtt_check(void)
{
    l610_mqtt_state_t mqtt_state = l610_mqtt_get_state();
    bool connected = (mqtt_state == MQTT_STATE_CONNECTED);
    char resp_buf[256];
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        const char *state_str = "disconnected";
        if (mqtt_state == MQTT_STATE_CONNECTED) state_str = "connected";
        else if (mqtt_state == MQTT_STATE_CONNECTING) state_str = "connecting";
        else if (mqtt_state == MQTT_STATE_DISCONNECTING) state_str = "disconnecting";
        else if (mqtt_state == MQTT_STATE_RECONNECTING) state_str = "reconnecting";
        else if (mqtt_state == MQTT_STATE_ERROR) state_str = "error";
        cJSON_AddStringToObject(resp, "type", "l610_mqtt_check_result");
        cJSON_AddBoolToObject(resp, "connected", connected);
        cJSON_AddStringToObject(resp, "mqtt_state", state_str);
        char *js = cJSON_PrintUnformatted(resp);
        if (js) { strncpy(resp_buf, js, sizeof(resp_buf) - 1); free(js); }
        cJSON_Delete(resp);
    }
    uart_handler_1_send_json(resp_buf);
}

// ===== JSON 命令分发 =====
static void uart1_process_line(const char *json_str)
{
    cJSON *json_obj = cJSON_Parse(json_str);
    if (!json_obj) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"INVALID_JSON\",\"msg\":\"JSON parse error\"}");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(json_obj, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"MISSING_FIELD\",\"msg\":\"Missing cmd field\"}");
        cJSON_Delete(json_obj);
        return;
    }

    const char *cmd_str = cmd_item->valuestring;

    // WS63 命令 → BE 命令映射表
    static const struct { const char *ws63_cmd; be_cmd_t be_cmd; } cmd_map[] = {
        {"register",         BE_CMD_REGISTER},
        {"inventory",        BE_CMD_INVENTORY},
        {"outbound",         BE_CMD_OUTBOUND},
        {"capture",          BE_CMD_CAPTURE_FRONT},
        {"delete",           BE_CMD_DELETE},
        {"cancel",           BE_CMD_CANCEL},
        {"list_assets",      BE_CMD_LIST_ASSETS},
        {"list_assets_page", BE_CMD_LIST_ASSETS_PAGE},
        {"asset_list_page",  BE_CMD_LIST_ASSETS_PAGE},
        {"get_asset",        BE_CMD_GET_ASSET},
        {"sys_info",         BE_CMD_SYS_INFO},
        {"ping",             BE_CMD_PING},
    };

    for (int i = 0; i < sizeof(cmd_map)/sizeof(cmd_map[0]); i++) {
        if (strcmp(cmd_str, cmd_map[i].ws63_cmd) == 0) {
            be_cmd_t be_cmd = cmd_map[i].be_cmd;
            // capture 命令需要解析 view 字段
            if (strcmp(cmd_str, "capture") == 0) {
                cJSON *view_item = cJSON_GetObjectItem(json_obj, "view");
                if (view_item && cJSON_IsString(view_item)) {
                    if (strcmp(view_item->valuestring, "side") == 0) be_cmd = BE_CMD_CAPTURE_SIDE;
                    else if (strcmp(view_item->valuestring, "top") == 0) be_cmd = BE_CMD_CAPTURE_TOP;
                }
            }

            cJSON *tag_item = cJSON_GetObjectItem(json_obj, "tag_id");
            const char *tag_id = tag_item ? tag_item->valuestring : NULL;

            char *params = cJSON_PrintUnformatted(json_obj);
            be_execute(BE_CHANNEL_UART1_JSON, be_cmd, tag_id, params);
            free(params);
            cJSON_Delete(json_obj);
            return;
        }
    }

    // L610 4G 命令（不经过 business_executor）
    if (strcmp(cmd_str, "mqtt_connect") == 0) {
        uart1_handle_l610_connect(json_obj);
    } else if (strcmp(cmd_str, "mqtt_disconnect") == 0) {
        uart1_handle_l610_disconnect(json_obj);
    } else if (strcmp(cmd_str, "mqtt_publish") == 0) {
        uart1_handle_l610_publish(json_obj);
    } else if (strcmp(cmd_str, "l610_status") == 0) {
        uart1_handle_l610_status();
    } else if (strcmp(cmd_str, "l610_at") == 0) {
        uart1_handle_l610_at(json_obj);
    } else if (strcmp(cmd_str, "l610_mqtt_check") == 0) {
        uart1_handle_l610_mqtt_check();
    } else {
        ESP_LOGE(TAG, "Unknown command: %s", cmd_str);
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"UNKNOWN_CMD\",\"msg\":\"Unknown command\"}");
    }

    cJSON_Delete(json_obj);
}

// ===== UART1 接收任务 =====
static void uart1_recv_task(void *pvParameters)
{
    // 注意：此任务不注册 WDT，因为同步处理 L610 AT 命令可能阻塞数秒
    // 如需 WDT 保护，应在 L610 AT 操作前后手动 esp_task_wdt_reset()
    uint8_t *data = (uint8_t *)malloc(WS63_UART_BUF_SIZE);
    char *line_buf = (char *)malloc(WS63_UART_BUF_SIZE);
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(WS63_UART_NUM, data, WS63_UART_BUF_SIZE, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (data[i] == '\n' || data[i] == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received from WS63: %s", line_buf);
                        uart1_process_line(line_buf);
                        line_pos = 0;
                    }
                } else {
                    if (line_pos < WS63_UART_BUF_SIZE - 1)
                        line_buf[line_pos++] = data[i];
                }
            }
        }
        vTaskDelay(1);
    }
    free(line_buf);
    free(data);
    vTaskDelete(NULL);
}

// ===== on_event 回调（JSON 格式化输出）=====
void uart_handler_1_on_event(be_event_t event, const void *data)
{
    switch (event) {
        case BE_EVT_HARDWARE_READY: {
            const be_hardware_ready_t *r = (const be_hardware_ready_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "capture_progress");
            cJSON_AddStringToObject(msg, "tag_id", r->tag_id);
            cJSON_AddStringToObject(msg, "view", "none");
            char step_str[16];
            snprintf(step_str, sizeof(step_str), "0/%d", r->total_views);
            cJSON_AddStringToObject(msg, "step", step_str);
            cJSON_AddStringToObject(msg, "status", "ready");
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_CAPTURE_PROGRESS: {
            const be_capture_progress_t *p = (const be_capture_progress_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "capture_progress");
            cJSON_AddStringToObject(msg, "tag_id", p->tag_id);
            cJSON_AddStringToObject(msg, "view",
                p->view_index == 0 ? "front" : (p->view_index == 1 ? "side" : "top"));
            char step[24];
            snprintf(step, sizeof(step), "%d/%d", p->view_index + 1, p->total_steps);
            cJSON_AddStringToObject(msg, "step", step);
            cJSON_AddStringToObject(msg, "status", "ok");
            cJSON_AddNumberToObject(msg, "blur_score", p->blur_score);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_TASK_DONE: {
            const be_task_done_t *d = (const be_task_done_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "task_done");
            cJSON_AddStringToObject(msg, "task",
                d->task == BE_CMD_REGISTER ? "register" :
                d->task == BE_CMD_INVENTORY ? "inventory" :
                d->task == BE_CMD_OUTBOUND ? "outbound" :
                d->task == BE_CMD_DELETE ? "delete" :
                d->task == BE_CMD_PING ? "ping" : "unknown");
            cJSON_AddStringToObject(msg, "tag_id", d->tag_id);
            cJSON_AddStringToObject(msg, "result", d->result ? d->result : "success");
            cJSON_AddStringToObject(msg, "item_name", d->item_name);
            cJSON_AddNumberToObject(msg, "quantity", d->quantity);
            if (d->task == BE_CMD_INVENTORY || d->task == BE_CMD_OUTBOUND) {
                cJSON_AddBoolToObject(msg, "is_match", d->is_match);
                cJSON_AddNumberToObject(msg, "confidence", d->confidence);
                cJSON_AddNumberToObject(msg, "threshold", d->threshold);
            }
            if (d->task == BE_CMD_OUTBOUND) {
                cJSON_AddNumberToObject(msg, "original_qty", d->previous_qty);
                cJSON_AddNumberToObject(msg, "remove_qty", d->remove_qty);
                cJSON_AddNumberToObject(msg, "remaining_qty", d->quantity);
            }
            if (d->is_verify_mode) {
                cJSON_AddNumberToObject(msg, "previous_qty", d->previous_qty);
                cJSON_AddNumberToObject(msg, "added_qty", d->quantity - d->previous_qty);
                cJSON_AddNumberToObject(msg, "new_qty", d->quantity);
            }
            cJSON_AddBoolToObject(msg, "is_overwrite", d->is_overwrite);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_ERROR: {
            const be_error_info_t *e = (const be_error_info_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "error");
            cJSON_AddStringToObject(msg, "code", "ERR_GENERIC");
            cJSON_AddStringToObject(msg, "msg", e->error_msg ? e->error_msg : "Unknown error");
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_ASSET_INFO: {
            const be_asset_info_t *a = (const be_asset_info_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "asset_info");
            cJSON_AddStringToObject(msg, "task", "outbound");
            cJSON_AddStringToObject(msg, "tag_id", a->tag_id);
            cJSON_AddStringToObject(msg, "item_name", a->item_name);
            char area_str[2] = {a->storage_area, '\0'};
            cJSON_AddStringToObject(msg, "storage_area", area_str);
            cJSON_AddNumberToObject(msg, "quantity", a->quantity);
            cJSON_AddNumberToObject(msg, "remove_qty", a->remove_qty);
            cJSON_AddNumberToObject(msg, "remaining_qty", a->remaining_qty);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_PONG_RESULT: {
            const be_pong_t *p = (const be_pong_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "pong");
            cJSON_AddBoolToObject(msg, "camera_ready", p->camera_ready);
            cJSON_AddBoolToObject(msg, "storage_ready", p->storage_ready);
            cJSON_AddNumberToObject(msg, "free_heap", p->free_heap);
            cJSON_AddStringToObject(msg, "state", p->state_str);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_SYS_INFO_RESULT: {
            const be_sys_info_t *s = (const be_sys_info_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "sys_info");
            cJSON_AddBoolToObject(msg, "camera_ready", s->camera_ready);
            cJSON_AddBoolToObject(msg, "storage_ready", s->storage_ready);
            cJSON_AddNumberToObject(msg, "free_heap", s->free_heap);
            cJSON_AddNumberToObject(msg, "storage_total_mb", s->storage_total_mb);
            cJSON_AddNumberToObject(msg, "storage_free_mb", s->storage_free_mb);
            cJSON_AddStringToObject(msg, "state", s->state_str);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_ASSET_LIST_RESULT: {
            const be_asset_list_t *l = (const be_asset_list_t *)data;
            uart_handler_1_send_json(l->json_payload);
            break;
        }
        case BE_EVT_ASSET_DETAIL: {
            const be_asset_detail_t *d = (const be_asset_detail_t *)data;
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "asset_detail");
            cJSON_AddStringToObject(msg, "tag_id", d->tag_id);
            cJSON_AddBoolToObject(msg, "found", d->found);
            if (d->found) {
                cJSON_AddStringToObject(msg, "item_name", d->item_name);
                char area_str[2] = {d->storage_area, '\0'};
                cJSON_AddStringToObject(msg, "storage_area", area_str);
                cJSON_AddNumberToObject(msg, "quantity", d->quantity);
            }
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        default:
            break;
    }
}

// ===== 初始化 =====
void uart_handler_1_init(void)
{
    uart1_uart_init();
    l610_manager_register_send_func(uart_handler_1_send_json);

    xTaskCreate(uart1_recv_task, "uart1_recv_task", 16384, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART1 WS63 handler initialized");
}
