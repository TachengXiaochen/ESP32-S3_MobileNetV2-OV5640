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

typedef struct {
    char     host[128];
    uint16_t port;
    uint8_t  clean_session;
    uint16_t keepalive;
    char     client_id[128];
    char     username[128];
    char     password[128];
    uint8_t  qos;
    uint8_t  retain;
} l610_mqtt_config_t;

/** @brief 默认 MQTT 端口（与 Kconfig EMQX MQTTS 一致） */
#define L610_MQTT_DEFAULT_PORT  L610_MQTT_BROKER_PORT

esp_err_t l610_mqtt_set_user(const char *client_id_str,
                              const char *username,
                              const char *password);

esp_err_t l610_mqtt_connect(const char *host, uint16_t port,
                             uint8_t clean_session, uint16_t keepalive,
                             int timeout_sec);

esp_err_t l610_mqtt_publish(const char *topic, const char *payload,
                             uint8_t qos, uint8_t retain,
                             int timeout_sec);

esp_err_t l610_mqtt_disconnect(int timeout_sec);

l610_mqtt_state_t l610_mqtt_get_state(void);

void l610_mqtt_set_state(l610_mqtt_state_t state);

void l610_mqtt_cleanup(void);

void l610_mqtt_urc_handler(const char *urc_line);

/** 上次 mqtt_connect 成功使用的 Broker（供断线重连） */
const char *l610_mqtt_get_connected_host(void);
uint16_t l610_mqtt_get_connected_port(void);

#ifdef __cplusplus
}
#endif

#endif /* L610_MQTT_H */
