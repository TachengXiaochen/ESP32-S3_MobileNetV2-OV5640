#ifndef L610_DRIVER_H
#define L610_DRIVER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include "l610_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化UART2并连接L610模块
 * 
 * 默认引脚： GPIO19(TX), GPIO20(RX), 115200 8N1
 * 安装UART驱动 → 设置引脚 → 安装模式检测
 * 
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t l610_driver_init(void);

/**
 * @brief 发送AT指令并同步等待应答
 * 
 * 自动追加 \r 结尾. 阻塞直到收到 OK/ERROR 或超时.
 * response_buf 会包含应答全文 (含OK/ERROR行).
 * 
 * @param cmd            AT指令 (不含 \r)
 * @param response_buf   应答缓冲区
 * @param buf_size       缓冲区大小
 * @param timeout_ms     超时(毫秒)
 * @return esp_err_t     ESP_OK=成功, ESP_ERR_TIMEOUT=超时
 */
esp_err_t l610_at_send(const char *cmd, char *response_buf,
                       size_t buf_size, uint32_t timeout_ms);

/**
 * @brief 发送AT指令并等待包含指定关键词的应答
 * 
 * 例如等待 "+MQTTOPEN: 1,1" 关键词
 * 
 * @param cmd            AT指令 (不含 \r)
 * @param keyword        期望关键词
 * @param response_buf   应答缓冲区
 * @param buf_size       缓冲区大小
 * @param timeout_ms     超时(毫秒)
 * @return esp_err_t     ESP_OK=找到关键词, ESP_ERR_TIMEOUT=超时
 */
esp_err_t l610_at_send_expect(const char *cmd, const char *keyword,
                              char *response_buf, size_t buf_size,
                              uint32_t timeout_ms);

/**
 * @brief 注册URC异步回调
 * 
 * URC解析器会在后台任务中识别 `+MQTTOPEN:` `+MQTTPUB:` 
 * `+MQTTBREAK:` `+MQTTMSG:` 等行, 并调用此回调.
 * 
 * @param callback  回调函数, 传入原始的URC行文本. 传NULL取消注册.
 */
void l610_register_urc_callback(l610_urc_cb_t callback);

/**
 * @brief 清除UART接收缓冲区
 */
void l610_uart_flush(void);

/**
 * @brief 检查L610模块是否就绪 (上次AT通信正常)
 */
bool l610_is_ready(void);

/**
 * @brief 获取模块状态文本描述
 */
const char *l610_status_str(void);

#ifdef __cplusplus
}
#endif

#endif /* L610_DRIVER_H */