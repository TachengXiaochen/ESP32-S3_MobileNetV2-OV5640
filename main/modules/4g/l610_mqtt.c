#include "l610_mqtt.h"
#include "l610_driver.h"
#include "esp_log.h"
#include "string.h"
#include "stdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "L610_MQTT";

// ========== 内部状态 ==========
static l610_mqtt_state_t g_mqtt_state = MQTT_STATE_DISCONNECTED;
static bool g_user_set = false;        // AT+MQTTUSER 是否已设置
static char g_current_host[128] = {0};
static uint16_t g_current_port = 0;

// ========== 信号量: 同步等待异步URC ==========
static SemaphoreHandle_t g_mqtt_open_sem = NULL;
static SemaphoreHandle_t g_mqtt_pub_sem  = NULL;
static SemaphoreHandle_t g_mqtt_close_sem = NULL;
static bool g_mqtt_open_result = false;  // true=成功, false=失败
static bool g_mqtt_pub_result  = false;
static bool g_mqtt_close_result = false;

// ========== 内部函数 ==========

static esp_err_t l610_wait_network_attached(void)
{
    char resp[128];

    for (int i = 0; i < 30; i++) {
        esp_err_t ret = l610_at_send("AT+CGATT?", resp, sizeof(resp),
                                     L610_AT_CGATT_TIMEOUT);
        if (ret == ESP_OK && strstr(resp, "+CGATT: 1") != NULL) {
            ESP_LOGI(TAG, "GPRS attached (CGATT=1)");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "CGATT attach timeout");
    return ESP_ERR_TIMEOUT;
}

static bool payload_needs_raw_mode(const char *topic, const char *payload, size_t len)
{
    if (len > L610_MQTT_STRING_SAFE_MAX) {
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        if (payload[i] == '"' || payload[i] == '\\') {
            return true;
        }
    }
    size_t est = strlen("AT+MQTTPUB=1,\"") + strlen(topic) + 16 + len + 4;
    return est >= L610_UART_BUF_SIZE - 1;
}

/**
 * @brief URC回调: 解析MQTT相关URC并更新状态/信号量
 */
void l610_mqtt_urc_handler(const char *urc_line)
{
    if (!urc_line) return;

    // +MQTTOPEN: 1,1 → 连接成功
    if (strstr(urc_line, "+MQTTOPEN:") != NULL) {
        if (strstr(urc_line, ",1") != NULL) {
            g_mqtt_state = MQTT_STATE_CONNECTED;
            g_mqtt_open_result = true;
            ESP_LOGI(TAG, "MQTT connected to %s:%d",
                     g_current_host, g_current_port);
        } else {
            g_mqtt_state = MQTT_STATE_ERROR;
            g_mqtt_open_result = false;
            ESP_LOGE(TAG, "MQTT connect failed (URC)");
        }
        if (g_mqtt_open_sem) {
            xSemaphoreGive(g_mqtt_open_sem);
        }
        return;
    }

    // +MQTTPUB: 1,1 → 发布成功
    if (strstr(urc_line, "+MQTTPUB:") != NULL) {
        if (strstr(urc_line, ",1") != NULL) {
            g_mqtt_pub_result = true;
        } else {
            g_mqtt_pub_result = false;
            ESP_LOGW(TAG, "MQTT publish failed (URC)");
        }
        if (g_mqtt_pub_sem) {
            xSemaphoreGive(g_mqtt_pub_sem);
        }
        return;
    }

    // +MQTTCLOSE: 1,1 → 关闭成功
    if (strstr(urc_line, "+MQTTCLOSE:") != NULL) {
        g_mqtt_state = MQTT_STATE_DISCONNECTED;
        g_mqtt_close_result = true;
        if (g_mqtt_close_sem) {
            xSemaphoreGive(g_mqtt_close_sem);
        }
        return;
    }

    // +MQTTBREAK: 1,<cause> → 意外断开
    if (strstr(urc_line, "+MQTTBREAK:") != NULL) {
        g_mqtt_state = MQTT_STATE_DISCONNECTED;
        ESP_LOGW(TAG, "MQTT connection broken: %s", urc_line);
        return;
    }
}

// ========== 接口实现 ==========

esp_err_t l610_mqtt_set_user(const char *client_id_str,
                              const char *username,
                              const char *password)
{
    // 使用默认值
    if (!username)   username   = L610_MQTT_USERNAME;
    if (!password)   password   = L610_MQTT_PASSWORD;
    
    // ✅ 优先使用传入的client_id_str，否则使用空字符串（L610会用IMEI）
    if (!client_id_str || strlen(client_id_str) == 0) {
        client_id_str = L610_MQTT_CLIENT_ID_STR;
    }

    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd),
                     "AT+MQTTUSER=%s,\"%s\",\"%s\",\"%s\"",
                     L610_MQTT_CONN_ID, username, password, client_id_str);
    if (n >= (int)sizeof(cmd)) {
        ESP_LOGE(TAG, "cmd buffer too small for MQTTUSER");
        return ESP_ERR_INVALID_SIZE;
    }

    char resp[256];
    esp_err_t ret = l610_at_send(cmd, resp, sizeof(resp),
                                 L610_AT_MQTT_USER_TIMEOUT);
    if (ret == ESP_OK) {
        g_user_set = true;
        ESP_LOGI(TAG, "MQTT user set: %s, ClientID=%s", username, client_id_str);
    }
    return ret;
}

esp_err_t l610_mqtt_connect(const char *host, uint16_t port,
                             uint8_t clean_session, uint16_t keepalive,
                             int timeout_sec)
{
    if (!host) return ESP_ERR_INVALID_ARG;

    // 如果未设置MQTTUSER, 先设置
    if (!g_user_set) {
        esp_err_t ret = l610_mqtt_set_user(NULL, NULL, NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // 保存当前连接信息
    strncpy(g_current_host, host, sizeof(g_current_host) - 1);
    g_current_port = port;

    esp_err_t ret = l610_wait_network_attached();
    if (ret != ESP_OK) {
        g_mqtt_state = MQTT_STATE_ERROR;
        return ret;
    }

    // 创建信号量 (如果尚未创建)
    if (!g_mqtt_open_sem) {
        g_mqtt_open_sem = xSemaphoreCreateBinary();
    }

    g_mqtt_state = MQTT_STATE_CONNECTING;
    g_mqtt_open_result = false;

    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd),
                     "AT+MQTTOPEN=%s,\"%s\",%d,%d,%d,%d",
                     L610_MQTT_CONN_ID, host, port,
                     clean_session, keepalive, L610_MQTT_USE_TLS);
    if (n >= (int)sizeof(cmd)) {
        ESP_LOGE(TAG, "cmd buffer too small for MQTTOPEN");
        return ESP_ERR_INVALID_SIZE;
    }

    char resp[256];
    ret = l610_at_send(cmd, resp, sizeof(resp),
                       L610_AT_MQTT_OPEN_TIMEOUT);
    if (ret != ESP_OK) {
        g_mqtt_state = MQTT_STATE_ERROR;
        return ret;
    }

    // 等待 +MQTTOPEN URC
    if (xSemaphoreTake(g_mqtt_open_sem, pdMS_TO_TICKS(timeout_sec * 1000))
        == pdTRUE) {
        return g_mqtt_open_result ? ESP_OK : ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "MQTT open URC timeout after %d sec", timeout_sec);
        g_mqtt_state = MQTT_STATE_ERROR;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t l610_mqtt_publish(const char *topic, const char *payload,
                             uint8_t qos, uint8_t retain,
                             int timeout_sec)
{
    if (!topic || !payload) return ESP_ERR_INVALID_ARG;

    size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > L610_MQTT_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Payload length invalid: %zu (max %d)", payload_len,
                 L610_MQTT_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    if (g_mqtt_state != MQTT_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Cannot publish: MQTT not connected (state=%d)",
                 g_mqtt_state);
        return ESP_ERR_INVALID_STATE;
    }

    if (!g_mqtt_pub_sem) {
        g_mqtt_pub_sem = xSemaphoreCreateBinary();
    }

    g_mqtt_pub_result = false;

    char resp[L610_UART_BUF_SIZE];
    esp_err_t ret;
    bool raw_mode = payload_needs_raw_mode(topic, payload, payload_len);

    if (raw_mode) {
        char cmd[256];
        int n = snprintf(cmd, sizeof(cmd),
                         "AT+MQTTPUB=%s,\"%s\",%d,%d,%zu",
                         L610_MQTT_CONN_ID, topic, qos, retain, payload_len);
        if (n >= (int)sizeof(cmd)) {
            ESP_LOGE(TAG, "cmd buffer too small for MQTTPUB raw");
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_LOGD(TAG, "MQTTPUB raw mode, %zu bytes", payload_len);
        ret = l610_at_send_then_raw(cmd, (const uint8_t *)payload, payload_len,
                                    resp, sizeof(resp), L610_AT_MQTT_PUB_TIMEOUT);
    } else {
        char cmd[L610_UART_BUF_SIZE];
        int n = snprintf(cmd, sizeof(cmd),
                         "AT+MQTTPUB=%s,\"%s\",%d,%d,\"%s\"",
                         L610_MQTT_CONN_ID, topic, qos, retain, payload);
        if (n >= (int)sizeof(cmd)) {
            ESP_LOGE(TAG, "cmd buffer too small for MQTTPUB (len=%d)", n);
            return ESP_ERR_INVALID_SIZE;
        }
        ret = l610_at_send(cmd, resp, sizeof(resp), L610_AT_MQTT_PUB_TIMEOUT);
    }

    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(g_mqtt_pub_sem, pdMS_TO_TICKS(timeout_sec * 1000))
        == pdTRUE) {
        return g_mqtt_pub_result ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGW(TAG, "MQTT publish URC timeout after %d sec", timeout_sec);
    return ESP_ERR_TIMEOUT;
}

esp_err_t l610_mqtt_disconnect(int timeout_sec)
{
    if (g_mqtt_state == MQTT_STATE_DISCONNECTED) {
        return ESP_OK;
    }

    if (!g_mqtt_close_sem) {
        g_mqtt_close_sem = xSemaphoreCreateBinary();
    }

    g_mqtt_state = MQTT_STATE_DISCONNECTING;
    g_mqtt_close_result = false;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+MQTTCLOSE=%s", L610_MQTT_CONN_ID);

    char resp[128];
    esp_err_t ret = l610_at_send(cmd, resp, sizeof(resp),
                                 L610_AT_MQTT_CLOSE_TIMEOUT);
    if (ret != ESP_OK) {
        g_mqtt_state = MQTT_STATE_ERROR;
        return ret;
    }

    if (xSemaphoreTake(g_mqtt_close_sem, pdMS_TO_TICKS(timeout_sec * 1000))
        == pdTRUE) {
        return g_mqtt_close_result ? ESP_OK : ESP_FAIL;
    } else {
        g_mqtt_state = MQTT_STATE_ERROR;
        return ESP_ERR_TIMEOUT;
    }
}

l610_mqtt_state_t l610_mqtt_get_state(void)
{
    return g_mqtt_state;
}

void l610_mqtt_set_state(l610_mqtt_state_t state)
{
    g_mqtt_state = state;
}

/**
 * @brief 清理MQTT资源（销毁信号量等）
 */
void l610_mqtt_cleanup(void)
{
    if (g_mqtt_open_sem) {
        vSemaphoreDelete(g_mqtt_open_sem);
        g_mqtt_open_sem = NULL;
    }
    if (g_mqtt_pub_sem) {
        vSemaphoreDelete(g_mqtt_pub_sem);
        g_mqtt_pub_sem = NULL;
    }
    if (g_mqtt_close_sem) {
        vSemaphoreDelete(g_mqtt_close_sem);
        g_mqtt_close_sem = NULL;
    }
    
    g_mqtt_state = MQTT_STATE_DISCONNECTED;
    g_user_set = false;
    memset(g_current_host, 0, sizeof(g_current_host));
    g_current_port = 0;
    
    ESP_LOGI(TAG, "L610 MQTT resources cleaned up");
}

const char *l610_mqtt_get_connected_host(void)
{
    return g_current_host[0] ? g_current_host : NULL;
}

uint16_t l610_mqtt_get_connected_port(void)
{
    return g_current_port;
}
