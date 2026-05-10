#ifndef L610_CONFIG_H
#define L610_CONFIG_H

#include "driver/gpio.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== UART2 引脚配置 (ESP32-S3 ↔ ADP-L610-Arduino J3) ==========
#define L610_UART_NUM              UART_NUM_2
#define L610_UART_TX_PIN           GPIO_NUM_19
#define L610_UART_RX_PIN           GPIO_NUM_20
#define L610_UART_BAUD_RATE        115200
#define L610_UART_BUF_SIZE         2048          // 接收缓冲区
#define L610_UART_TX_BUF_SIZE      1024          // 发送缓冲区
#define L610_UART_QUEUE_SIZE       20
#define L610_UART_PATTERN_CHR      '\n'          // URC行结束符

// ========== AT 指令超时配置 (毫秒) ==========
#define L610_AT_DEFAULT_TIMEOUT    5000          // 默认AT超时
#define L610_AT_CSQ_TIMEOUT        3000          // 信号质量查询
#define L610_AT_CGATT_TIMEOUT      10000         // 网络附着查询
#define L610_AT_MQTT_USER_TIMEOUT  3000          // MQTTUSER 超时
#define L610_AT_MQTT_OPEN_TIMEOUT  15000         // MQTTOPEN 超时(含网络注册)
#define L610_AT_MQTT_PUB_TIMEOUT   8000          // MQTTPUB 超时
#define L610_AT_MQTT_CLOSE_TIMEOUT 5000          // MQTTCLOSE 超时

// ========== 重试与重连配置 ==========
#define L610_AT_MAX_RETRY          3             // AT指令失败最大重试次数
#define L610_HEARTBEAT_INTERVAL_MS 30000         // 心跳检测间隔(ms)
#define L610_MQTT_RECONNECT_MAX    3             // MQTT最大重连次数
#define L610_MQTT_RECONNECT_DELAY_MS 5000        // 重连间隔(ms)
#define L610_LOST_THRESHOLD        3             // 连续超时次数 → 标记L610_OFF

// ========== MQTT 凭据 (硬编码, 见ThingsKit配置) ==========
#define L610_MQTT_CLIENT_ID        1
#define L610_MQTT_USERNAME         "IekgXZSavYJ6KEJFyvb4"
#define L610_MQTT_PASSWORD         ""
#define L610_MQTT_KEEPALIVE        60
#define L610_MQTT_CLEAN_SESSION    1
#define L610_MQTT_USE_TLS          0
#define L610_MQTT_QOS              1
#define L610_MQTT_RETAIN           0

// ========== 状态枚举 ==========
typedef enum {
    L610_STATE_OFF = 0,            // 模块未响应/断电
    L610_STATE_READY,              // AT通信正常
    L610_STATE_ERROR               // 连续超时/错误状态
} l610_module_state_t;

typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_DISCONNECTING,
    MQTT_STATE_RECONNECTING,
    MQTT_STATE_ERROR
} l610_mqtt_state_t;

// 状态上报结构体
typedef struct {
    l610_module_state_t module_state;
    l610_mqtt_state_t   mqtt_state;
    int                 signal_quality;   // CSQ值 0-31, 99=未知
    int                 consecutive_timeouts; // 连续超时计数
} l610_status_t;

// URC回调函数类型
typedef void (*l610_urc_cb_t)(const char *urc_line);

#ifdef __cplusplus
}
#endif

#endif /* L610_CONFIG_H */