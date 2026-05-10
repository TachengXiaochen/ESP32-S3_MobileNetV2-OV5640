#include "l610_manager.h"
#include "l610_driver.h"
#include "l610_mqtt.h"
#include "esp_log.h"
#include "string.h"
#include "stdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "L610_MGR";

// ========== 全局状态 ==========
static TaskHandle_t g_heartbeat_task = NULL;
static volatile bool g_manager_running = false;

// 缓存状态 (用于快速查询)
static l610_status_t g_cached_status = {
    .module_state  = L610_STATE_OFF,
    .mqtt_state    = MQTT_STATE_DISCONNECTED,
    .signal_quality = 99,
    .consecutive_timeouts = 0
};

// MQTT重连状态
static int g_reconnect_attempts = 0;

// WS63上行的协议输出函数 (由 protocol_handler 注入，避免循环依赖)
static void (*g_ws63_send_func)(const char *json_line) = NULL;

/**
 * @brief 注册WS63发送函数 (由protocol_handler调用)
 * 
 * 用于manager层主动向WS63上报L610状态变化
 * 
 * @param send_func 发送JSON Lines的函数指针
 */
void l610_manager_register_send_func(void (*send_func)(const char *));

/**
 * @brief 内部: 通过UART1向WS63上报消息
 */
static void send_to_ws63(const char *type, const char *json_fmt, ...)
{
    if (!g_ws63_send_func) return;

    char line[512];
    va_list args;
    va_start(args, json_fmt);
    vsnprintf(line, sizeof(line), json_fmt, args);
    va_end(args);

    g_ws63_send_func(line);
}

/**
 * @brief 内部: 解析AT+CSQ应答
 * 
 * +CSQ: <rssi>,<ber>
 * 返回rssi (0-31), 解析失败返回99
 */
static int parse_csq(const char *response)
{
    const char *p = strstr(response, "+CSQ: ");
    if (!p) return 99;

    p += 6;  // 跳过 "+CSQ: "
    int rssi = 0;
    if (sscanf(p, "%d", &rssi) == 1) {
        return rssi;
    }
    return 99;
}

// ========== URC 回调 ==========

/**
 * @brief L610 URC回调入口 (注册到l610_driver)
 * 
 * 收到MQTT相关的URC时:
 * - +MQTTOPEN:  → l610_mqtt_urc_handler (mqtt层处理)
 * - +MQTTPUB:   → l610_mqtt_urc_handler
 * - +MQTTCLOSE: → l610_mqtt_urc_handler
 * - +MQTTBREAK: → l610_mqtt_urc_handler + 触发重连逻辑
 * - +MQTTMSG:   → 预留
 */
static void urc_callback(const char *urc_line)
{
    if (!urc_line) return;

    // 先把URC传给mqtt层处理 (信号量/状态更新)
    l610_mqtt_urc_handler(urc_line);

    // +MQTTBREAK: 意外断开 → 尝试重连
    if (strstr(urc_line, "+MQTTBREAK:") != NULL) {
        ESP_LOGW(TAG, "MQTT broken, scheduling reconnect...");
        // 心跳任务中会自动重连
        l610_mqtt_set_state(MQTT_STATE_RECONNECTING);
        g_reconnect_attempts = 0;

        // 主动上报WS63
        send_to_ws63("l610_error",
                     "{\"type\":\"l610_error\","
                     "\"code\":\"L610_MQTT_LOST_CONNECTION\","
                     "\"msg\":\"MQTT connection lost: %s\"}",
                     urc_line);
    }
}

// ========== 心跳任务 ==========

static void heartbeat_task(void *arg)
{
    ESP_LOGI(TAG, "Heartbeat task started (interval=%d ms)",
             L610_HEARTBEAT_INTERVAL_MS);

    char resp[256];

    while (g_manager_running) {
        // --- Step 1: 检测模块是否响应 ---
        esp_err_t ret = l610_at_send("AT", resp, sizeof(resp),
                                     L610_AT_DEFAULT_TIMEOUT);
        if (ret != ESP_OK) {
            g_cached_status.consecutive_timeouts++;
            ESP_LOGW(TAG, "L610 not responding (%d/%d)",
                     g_cached_status.consecutive_timeouts,
                     L610_LOST_THRESHOLD);

            if (g_cached_status.consecutive_timeouts >= L610_LOST_THRESHOLD) {
                if (g_cached_status.module_state != L610_STATE_OFF) {
                    g_cached_status.module_state = L610_STATE_OFF;
                    g_cached_status.mqtt_state = MQTT_STATE_DISCONNECTED;
                    ESP_LOGE(TAG, "L610 module lost!");
                    send_to_ws63("l610_error",
                        "{\"type\":\"l610_error\","
                        "\"code\":\"L610_NOT_RESPONDING\","
                        "\"msg\":\"L610 module lost after %d timeouts\"}",
                        L610_LOST_THRESHOLD);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(L610_HEARTBEAT_INTERVAL_MS));
            continue;
        }

        // 模块正常响应
        if (g_cached_status.module_state != L610_STATE_READY) {
            g_cached_status.module_state = L610_STATE_READY;
            ESP_LOGI(TAG, "L610 module is ready");
        }
        g_cached_status.consecutive_timeouts = 0;

        // --- Step 2: 获取信号质量 ---
        ret = l610_at_send("AT+CSQ", resp, sizeof(resp),
                           L610_AT_CSQ_TIMEOUT);
        if (ret == ESP_OK) {
            int csq = parse_csq(resp);
            if (csq != g_cached_status.signal_quality) {
                g_cached_status.signal_quality = csq;
                ESP_LOGI(TAG, "Signal quality: %d/31", csq);
            }
        }

        // --- Step 3: 检查MQTT状态并处理重连 ---
        l610_mqtt_state_t mqtt_state = l610_mqtt_get_state();
        g_cached_status.mqtt_state = mqtt_state;

        if (mqtt_state == MQTT_STATE_RECONNECTING ||
            (mqtt_state == MQTT_STATE_DISCONNECTED &&
             g_cached_status.module_state == L610_STATE_READY &&
             g_reconnect_attempts > 0)) {

            if (g_reconnect_attempts < L610_MQTT_RECONNECT_MAX) {
                g_reconnect_attempts++;
                ESP_LOGI(TAG, "MQTT reconnecting... (%d/%d)",
                         g_reconnect_attempts, L610_MQTT_RECONNECT_MAX);

                esp_err_t conn_ret = l610_manager_reconnect_mqtt();
                if (conn_ret == ESP_OK) {
                    g_reconnect_attempts = 0;
                    ESP_LOGI(TAG, "MQTT reconnected successfully");
                    send_to_ws63("mqtt_connected",
                        "{\"type\":\"mqtt_connected\","
                        "\"state\":\"connected\"}");
                }
            } else {
                ESP_LOGE(TAG, "MQTT reconnect failed after %d attempts",
                         L610_MQTT_RECONNECT_MAX);
                g_cached_status.mqtt_state = MQTT_STATE_ERROR;
                g_reconnect_attempts = 0;
                send_to_ws63("l610_error",
                    "{\"type\":\"l610_error\","
                    "\"code\":\"L610_MQTT_CONNECT_FAIL\","
                    "\"msg\":\"MQTT reconnect failed after max retries\"}");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(L610_HEARTBEAT_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Heartbeat task stopped");
    vTaskDelete(NULL);
}

// ========== 接口实现 ==========

esp_err_t l610_manager_init(void)
{
    // 1. 初始化L610 UART驱动
    esp_err_t ret = l610_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "L610 driver init failed");
        return ret;
    }

    // 2. 注册URC回调 (链式: driver → manager → mqtt)
    l610_register_urc_callback(urc_callback);

    // 3. 检测模块是否在线
    char resp[128];
    ret = l610_at_send("AT", resp, sizeof(resp), L610_AT_DEFAULT_TIMEOUT);
    if (ret == ESP_OK) {
        g_cached_status.module_state = L610_STATE_READY;
        ESP_LOGI(TAG, "L610 module detected and ready");
    } else {
        g_cached_status.module_state = L610_STATE_OFF;
        ESP_LOGW(TAG, "L610 module not responding during init");
    }

    // 4. 初始化MQTT凭据 (预设置)
    ret = l610_mqtt_set_user(NULL, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT user set failed (will retry later)");
    }

    return ESP_OK;
}

esp_err_t l610_manager_start(void)
{
    if (g_manager_running) {
        ESP_LOGW(TAG, "Manager already running");
        return ESP_OK;
    }

    g_manager_running = true;
    g_reconnect_attempts = 0;

    BaseType_t ret = xTaskCreate(heartbeat_task, "l610_hb",
                                 4096, NULL, 5, &g_heartbeat_task);
    if (ret != pdTRUE) {
        g_manager_running = false;
        ESP_LOGE(TAG, "Failed to create heartbeat task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "L610 manager started");
    return ESP_OK;
}

esp_err_t l610_manager_stop(void)
{
    g_manager_running = false;
    if (g_heartbeat_task) {
        vTaskDelete(g_heartbeat_task);
        g_heartbeat_task = NULL;
    }
    
    // ✅ 清理MQTT资源（信号量等）
    extern void l610_mqtt_cleanup(void);
    l610_mqtt_cleanup();
    
    return ESP_OK;
}

esp_err_t l610_manager_get_status(l610_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    // 从缓存读取 (心跳任务实时更新)
    status->module_state       = g_cached_status.module_state;
    status->mqtt_state         = l610_mqtt_get_state();
    status->signal_quality     = g_cached_status.signal_quality;
    status->consecutive_timeouts = g_cached_status.consecutive_timeouts;

    return ESP_OK;
}

int l610_manager_get_signal_quality(void)
{
    char resp[128];
    esp_err_t ret = l610_at_send("AT+CSQ", resp, sizeof(resp),
                                 L610_AT_CSQ_TIMEOUT);
    if (ret != ESP_OK) return 99;

    return parse_csq(resp);
}

esp_err_t l610_manager_reconnect_mqtt(void)
{
    // 使用硬编码的ThingsKit凭据重连
    return l610_mqtt_connect("demo.thingskit.com", 1883,
                             1, 60, 15);
}

void l610_manager_register_send_func(void (*send_func)(const char *))
{
    g_ws63_send_func = send_func;
}