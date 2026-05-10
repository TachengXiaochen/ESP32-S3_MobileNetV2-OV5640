#ifndef L610_MANAGER_H
#define L610_MANAGER_H

#include "esp_err.h"
#include "l610_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化L610管理器
 * 
 * 初始化UART2驱动 → 注册URC回调 → 检测模块是否存在(发送AT)
 * 
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t l610_manager_init(void);

/**
 * @brief 启动心跳检测任务 (FreeRTOS任务)
 * 
 * 每30秒检查L610状态:
 * - 发送 AT 检测模块是否响应
 * - 发送 AT+CSQ 获取信号质量
 * - 若MQTT意外断开, 尝试自动重连
 * - 若模块连续超时超过阈值, 上报状态变化
 * 
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t l610_manager_start(void);

/**
 * @brief 停止心跳任务
 */
esp_err_t l610_manager_stop(void);

/**
 * @brief 查询L610整体状态
 * 
 * @param[out] status 状态结构体指针
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t l610_manager_get_status(l610_status_t *status);

/**
 * @brief 发送AT+CSQ获取信号质量
 * 
 * @return int 信号质量(0-31), 失败返回99
 */
int l610_manager_get_signal_quality(void);

/**
 * @brief 尝试重新连接MQTT (用于重连逻辑)
 * 
 * @return esp_err_t 连接结果
 */
esp_err_t l610_manager_reconnect_mqtt(void);

#ifdef __cplusplus
}
#endif

#endif /* L610_MANAGER_H */