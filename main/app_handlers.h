#ifndef APP_HANDLERS_H
#define APP_HANDLERS_H

#include "modules/system/executor/business_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

// FreeRTOS 任务入口（供 main.c xTaskCreate 调用）
void camera_ai_task(void *pvParameters);

// business_executor 输出回调（供 main.c be_init 注册）
void be_output_callback(be_channel_t channel, be_event_t event, const void *data);

#ifdef __cplusplus
}
#endif

#endif /* APP_HANDLERS_H */
