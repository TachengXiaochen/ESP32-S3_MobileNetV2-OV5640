#include "l610_driver.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "esp_timer.h"

static const char *TAG = "L610_DRV";

// ========== 全局状态 ==========
static bool g_ready = false;
static l610_urc_cb_t g_urc_callback = NULL;
static TaskHandle_t g_uart_rx_task = NULL;
static int g_consecutive_timeouts = 0;

// ========== URC 关键词列表 (用于rx任务识别) ==========
static const char * const urc_keywords[] = {
    "+MQTTOPEN:",
    "+MQTTPUB:",
    "+MQTTCLOSE:",
    "+MQTTBREAK:",
    "+MQTTMSG:",
    "+CME ERROR:",
    "+CMS ERROR:",
    NULL  // 哨兵
};

/**
 * @brief 检测一行文本是否包含URC关键词
 */
static bool is_urc_line(const char *line)
{
    for (int i = 0; urc_keywords[i] != NULL; i++) {
        if (strstr(line, urc_keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/**
 * @brief UART RX 后台任务: 读取原始数据, 按行分割, 分发URC
 */
static void uart_rx_task(void *arg)
{
    uart_port_t uart_num = L610_UART_NUM;
    uint8_t *data = malloc(L610_UART_BUF_SIZE);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate UART RX buffer");
        vTaskDelete(NULL);
        return;
    }

    // 用于行缓存
    static char line_buf[L610_UART_BUF_SIZE];
    static int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(uart_num, data, L610_UART_BUF_SIZE,
                                  pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len; i++) {
            char ch = (char)data[i];
            if (ch == '\n') {
                line_buf[line_pos] = '\0';
                // 去尾 \r
                if (line_pos > 0 && line_buf[line_pos - 1] == '\r') {
                    line_buf[line_pos - 1] = '\0';
                }

                // 检查是否为URC行
                if (line_pos > 0 && is_urc_line(line_buf)) {
                    ESP_LOGI(TAG, "URC: %s", line_buf);
                    if (g_urc_callback) {
                        g_urc_callback(line_buf);
                    }
                }

                line_pos = 0;
            } else {
                if (line_pos < (int)sizeof(line_buf) - 1) {
                    line_buf[line_pos++] = ch;
                }
            }
        }
    }

    free(data);
    vTaskDelete(NULL);
}

// ========== 接口实现 ==========

esp_err_t l610_driver_init(void)
{
    uart_port_t uart_num = L610_UART_NUM;

    // UART参数配置
    uart_config_t uart_config = {
        .baud_rate  = L610_UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret;

    ret = uart_param_config(uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(uart_num, L610_UART_TX_PIN, L610_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(uart_num, L610_UART_BUF_SIZE,
                              L610_UART_TX_BUF_SIZE, L610_UART_QUEUE_SIZE,
                              NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 创建URC检测任务
    xTaskCreate(uart_rx_task, "l610_uart_rx", 4096, NULL, 5,
                &g_uart_rx_task);

    g_ready = true;
    g_consecutive_timeouts = 0;
    ESP_LOGI(TAG, "L610 UART2 initialized (TX=%d, RX=%d, 115200)",
             L610_UART_TX_PIN, L610_UART_RX_PIN);

    return ESP_OK;
}

esp_err_t l610_at_send(const char *cmd, char *response_buf,
                       size_t buf_size, uint32_t timeout_ms)
{
    if (!g_ready) {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_port_t uart_num = L610_UART_NUM;

    // ✅ 新增：自动重试机制（最多L610_AT_MAX_RETRY次）
    for (int retry = 0; retry < L610_AT_MAX_RETRY; retry++) {
        // 先flush接收缓冲区, 清除残留数据
        uart_flush(uart_num);

        // 发送AT指令 (自动追加 \r)
        size_t cmd_len = strlen(cmd);
        char *send_buf = malloc(cmd_len + 2);
        if (!send_buf) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(send_buf, cmd, cmd_len);
        send_buf[cmd_len]     = '\r';
        send_buf[cmd_len + 1] = '\0';

        uart_write_bytes(uart_num, send_buf, cmd_len + 1);
        free(send_buf);

        // 等待应答 (包含OK/ERROR行)
        int total_read = 0;
        int64_t start_us = esp_timer_get_time();
        int64_t timeout_us = (int64_t)timeout_ms * 1000;

        while ((esp_timer_get_time() - start_us) < timeout_us) {
            int available = 0;
            uart_get_buffered_data_len(uart_num, (size_t *)&available);
            if (available > 0) {
                int to_read = available;
                if (total_read + to_read > (int)buf_size - 1) {
                    to_read = (int)buf_size - 1 - total_read;
                }
                if (to_read > 0) {
                    int len = uart_read_bytes(uart_num,
                                              (uint8_t *)&response_buf[total_read],
                                              to_read, pdMS_TO_TICKS(10));
                    if (len > 0) {
                        total_read += len;
                        response_buf[total_read] = '\0';

                        // 检查是否包含 OK 或 ERROR
                        if (strstr(response_buf, "OK\r\n") != NULL ||
                            strstr(response_buf, "OK\n") != NULL ||
                            strstr(response_buf, "ERROR\r\n") != NULL ||
                            strstr(response_buf, "ERROR\n") != NULL) {
                            g_consecutive_timeouts = 0;
                            if (retry > 0) {
                                ESP_LOGI(TAG, "AT command succeeded after %d retries: %s", 
                                         retry, cmd);
                            }
                            return ESP_OK;
                        }
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // 本次尝试超时
        if (retry < L610_AT_MAX_RETRY - 1) {
            ESP_LOGD(TAG, "AT timeout (retry %d/%d): %s", 
                     retry + 1, L610_AT_MAX_RETRY - 1, cmd);
            vTaskDelay(pdMS_TO_TICKS(200));  // 短暂延迟后重试
        }
    }

    // 所有重试均失败
    g_consecutive_timeouts++;
    response_buf[0] = '\0';
    
    // 根据连续超时次数调整日志级别
    if (g_consecutive_timeouts >= L610_LOST_THRESHOLD) {
        ESP_LOGW(TAG, "AT timeout after %lu ms (consecutive=%d/%d, LOST): %s",
                 (unsigned long)timeout_ms, g_consecutive_timeouts, 
                 L610_LOST_THRESHOLD, cmd);
    } else if (g_consecutive_timeouts >= 2) {
        ESP_LOGW(TAG, "AT timeout after %lu ms (consecutive=%d): %s",
                 (unsigned long)timeout_ms, g_consecutive_timeouts, cmd);
    } else {
        ESP_LOGD(TAG, "AT timeout (1st): %s", cmd);
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t l610_at_send_expect(const char *cmd, const char *keyword,
                              char *response_buf, size_t buf_size,
                              uint32_t timeout_ms)
{
    esp_err_t ret = l610_at_send(cmd, response_buf, buf_size, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    if (keyword && strstr(response_buf, keyword) == NULL) {
        ESP_LOGW(TAG, "Expected keyword '%s' not found in response: %s",
                 keyword, response_buf);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

void l610_register_urc_callback(l610_urc_cb_t callback)
{
    g_urc_callback = callback;
}

void l610_uart_flush(void)
{
    uart_flush(L610_UART_NUM);
}

bool l610_is_ready(void)
{
    return g_ready && (g_consecutive_timeouts < L610_LOST_THRESHOLD);
}

const char *l610_status_str(void)
{
    if (!g_ready) return "NOT_INIT";
    if (g_consecutive_timeouts >= L610_LOST_THRESHOLD) return "LOST";
    return "READY";
}