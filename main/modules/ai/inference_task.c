/**
 * @file inference_task.c
 * @brief AI 推理任务 — 自适应帧采集 + 特征融合
 *
 * 从 main.c 拆分。接收 xInferenceQueue 中的推理任务，执行
 * 自适应帧数采集（默认1帧，边缘清晰度补充至最多3帧），
 * 全部视图完成后触发 CMD_INFERENCE_TRIGGER。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"

#include "inference_task.h"
#include "main.h"
#include "camera_module.h"
#include "feature_processor.h"
#include "blur_detection.h"

static const char *TAG = "camera_ai";
#define SAFE_WDT_RESET()    esp_task_wdt_reset()

// ========== 推理任务 ==========
void inference_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    inference_job_t job;
    const int MAX_FRAMES = 3;              // 边缘情况下最多采3帧（正常1帧）
    while (1) {
        if (xQueueReceive(xInferenceQueue, &job, pdMS_TO_TICKS(2000))) {
            SAFE_WDT_RESET();
            float *fp = NULL; const char *vn = NULL;
            if (job.view_cmd == CMD_CAPTURE_FRONT) { fp = g_front_feature; vn = "Front"; }
            else if (job.view_cmd == CMD_CAPTURE_SIDE) { fp = g_side_feature; vn = "Side"; }
            else { fp = g_top_feature; vn = "Top"; }
            ESP_LOGI(TAG, "[INF] %s start", vn);

            // 自适应帧数：默认1帧，边缘清晰度时补充到最多3帧
            feature_processor_reset_frame_count();
            int fc = 0;
            float blur_var = 0.0f;
            bool need_more = false;

            // --- 第1帧 ---
            if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                float sf[FEATURE_VEC_SIZE];
                if (camera_module_capture_and_process(sf, FEATURE_VEC_SIZE, &blur_var)) {
                    feature_processor_add_frame(sf, FEATURE_VEC_SIZE);
                    fc++;
                    // 判断清晰度：边缘 → 需要补充帧
                    if (blur_var < BLUR_CONFIDENT_THRESHOLD) {
                        need_more = true;
                        ESP_LOGI(TAG, "[INF] %s blur=%.1f marginal, supplementing", vn, (double)blur_var);
                    } else {
                        ESP_LOGI(TAG, "[INF] %s blur=%.1f confident, 1 frame OK", vn, (double)blur_var);
                    }
                }
                xSemaphoreGive(xCameraMutex);
            }
            esp_task_wdt_reset();

            // --- 补充帧（仅边缘情况） ---
            if (need_more) {
                for (int i = 1; i < MAX_FRAMES && fc < MAX_FRAMES; i++) {
                    if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        float sf[FEATURE_VEC_SIZE];
                        if (camera_module_capture_and_process(sf, FEATURE_VEC_SIZE, NULL)) {
                            feature_processor_add_frame(sf, FEATURE_VEC_SIZE);
                            fc++;
                        }
                        xSemaphoreGive(xCameraMutex);
                    }
                    esp_task_wdt_reset();
                }
            }

            // --- 融合输出 ---
            if (g_inference_cancelled) {
                ESP_LOGI(TAG, "[INF] %s cancelled, discarding", vn);
                g_inference_cancelled = false;
                esp_task_wdt_reset();
                continue;
            }
            if (fc > 0 && feature_processor_get_fused_feature(fp, FEATURE_VEC_SIZE))
                ESP_LOGI(TAG, "[INF] %s fusion %d frames", vn, fc);
            else ESP_LOGW(TAG, "[INF] %s no frames", vn);

            g_views_processed++;
            if (g_views_processed >= g_total_views) {
                system_msg_t tm = {0}; tm.cmd = CMD_INFERENCE_TRIGGER;
                snprintf(tm.tag_id, sizeof(tm.tag_id), "%s", job.tag_id);
                xQueueSend(xSystemQueue, &tm, portMAX_DELAY);
                g_views_enqueued = 0; g_views_processed = 0; g_total_views = 0;
            }
            esp_task_wdt_reset();
        } else { SAFE_WDT_RESET(); }
    }
}
