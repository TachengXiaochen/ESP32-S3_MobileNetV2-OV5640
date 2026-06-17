#ifndef INFERENCE_TASK_H
#define INFERENCE_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

// FreeRTOS 推理任务入口
void inference_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif /* INFERENCE_TASK_H */
