#ifndef L610_MQTT_H
#define L610_MQTT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "l610_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== MQTT 配置结构体 ==========

/**
 * @brief MQTT连接配置结构体
 */
typedef struct {
    char     host[128];          /**< MQTT Broker地址 */
    uint16_t port;               /**< MQTT Broker端口 */
    uint8_t  clean_session;      /**< 清理会话标志 (0/1) */
    uint16_t keepalive;          /**< 心跳间隔(秒) */
    char     client_id[128];     /**< Client ID (可选, NULL取默认) */
    char     username[128];      /**< 用户名 (可选, NULL取默认) */
    char     password[128];      /**< 密码 (可选, NULL取默认) */
    uint8_t  qos;                /**< 默认发布QoS (0/1/2) */
    uint8_t  retain;             /**< 默认保留标志 (0/1) */
} l610_mqtt_config_t;

/** @brief ThingsKit默认MQTT端口 (非TLS) */
#define L610_MQTT_DEFAULT_PORT  1883

// ========== 函数接口 ==========

/**
 * @brief 设置MQTT用户凭据
 * 
 * AT+MQTTUSER=1,username,password,client_id_str
 * 
 * @param client_id_str  ClientID字符串, 例如 "WS63-AA:BB:CC:DD:EE:FF"
 *                       NULL则使用硬编码默认值
 * @param username       MQTT用户名, NULL使用硬编码
 * @param password       MQTT密码, NULL使用硬编码
 * @return esp_err_t     ESP_OK 成功
 */
esp_err_t l610_mqtt_set_user(const char *client_id_str,
                              const char *username,
                              const char *password);

/**
 * @brief 连接MQTT Broker
 * 
 * 内部执行: AT+MQTTUSER (若未设置) → AT+MQTTOPEN
 * 等待 +MQTTOPEN: 1,1 异步URC
 * 
 * @param host           Broker地址
 * @param port           端口
 * @param clean_session  0/1
 * @param keepalive      心跳间隔(秒)
 * @param timeout_sec    等待连接URC的超时(秒)
 * @return esp_err_t     ESP_OK=连接成功
 */
esp_err_t l610_mqtt_connect(const char *host, uint16_t port,
                             uint8_t clean_session, uint16_t keepalive,
                             int timeout_sec);

/**
 * @brief 发布消息到Topic
 * 
 * AT+MQTTPUB=1,topic,qos,retain,payload
 * 等待 +MQTTPUB: 1,1 异步URC
 * 
 * @param topic          MQTT主题
 * @param payload        JSON payload (注意转义)
 * @param qos            QoS 0/1/2
 * @param retain         retain 0/1
 * @param timeout_sec    等待发布URC的超时(秒)
 * @return esp_err_t     ESP_OK=发布成功
 */
esp_err_t l610_mqtt_publish(const char *topic, const char *payload,
                             uint8_t qos, uint8_t retain,
                             int timeout_sec);

/**
 * @brief 断开MQTT连接
 * 
 * AT+MQTTCLOSE=1
 * 
 * @param timeout_sec    超时(秒)
 * @return esp_err_t     成功
 */
esp_err_t l610_mqtt_disconnect(int timeout_sec);

/**
 * @brief 获取当前MQTT连接状态
 */
l610_mqtt_state_t l610_mqtt_get_state(void);

/**
 * @brief 设置MQTT状态 (由manager层使用)
 */
void l610_mqtt_set_state(l610_mqtt_state_t state);

/**
 * @brief 清理MQTT资源（销毁信号量等）
 * 
 * 在模块停止或反初始化时调用，释放所有动态创建的资源
 */
void l610_mqtt_cleanup(void);

/**
 * @brief 内部URC处理回调 (由driver层的URC分发调用)
 * 
 * 此函数注册为URC回调，解析MQTT相关URC并更新内部状态
 */
void l610_mqtt_urc_handler(const char *urc_line);

#ifdef __cplusplus
}
#endif

#endif /* L610_MQTT_H */