#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化Web服务器（WiFi SoftAP + SPIFFS + HTTP端点）
 *
 * 必须在 app_main() 中调用，在所有其他模块初始化完成后。
 * 内部会延迟3秒再启动WiFi，避免与SD卡DMA冲突。
 *
 * @return ESP_OK 成功
 */
esp_err_t web_server_init(void);

/**
 * @brief 反初始化Web服务器
 */
void web_server_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
