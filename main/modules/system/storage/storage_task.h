#ifndef STORAGE_TASK_H
#define STORAGE_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

// FreeRTOS 存储管理任务入口
void storage_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_TASK_H */
