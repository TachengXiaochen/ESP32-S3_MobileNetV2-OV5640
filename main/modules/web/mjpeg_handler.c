#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "camera_module.h"

static const char *TAG = "mjpeg";

// 引用 main.c 中的摄像头互斥锁
extern SemaphoreHandle_t xCameraMutex;

#define PART_BOUNDARY "frame"
#define STREAM_DELAY_MS 30

esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_OK;
    char part_buf[128];

    ESP_LOGI(TAG, "MJPEG stream client connected");

    // 设置 MJPEG multipart 响应头
    res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" PART_BOUNDARY);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t frame_count = 0;
    while (true) {
        uint8_t *jpeg_buf = NULL;
        size_t jpeg_len = 0;

        // 100ms 短超时 —— AI 管道持锁时直接跳过本帧
        if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool ok = camera_module_capture_jpeg(&jpeg_buf, &jpeg_len);
            xSemaphoreGive(xCameraMutex);

            if (ok && jpeg_buf && jpeg_len > 0) {
                // 构建 multipart 帧头
                int hdr_len = snprintf(part_buf, sizeof(part_buf),
                    "\r\n--" PART_BOUNDARY "\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %u\r\n\r\n",
                    (unsigned)jpeg_len);

                // 发送: 帧头 + JPEG 数据 + 帧尾
                if (httpd_resp_send_chunk(req, part_buf, hdr_len) != ESP_OK ||
                    httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_len) != ESP_OK ||
                    httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                    free(jpeg_buf);
                    ESP_LOGI(TAG, "Client disconnected after %lu frames", (unsigned long)frame_count);
                    res = ESP_FAIL;
                    break;
                }

                frame_count++;
                free(jpeg_buf);
            } else {
                // 捕获失败也要释放锁（已在上面释放），仅 debug 级别日志
                ESP_LOGD(TAG, "Frame capture skipped or failed");
            }
        }
        // 互斥锁忙则跳过此帧迭代

        vTaskDelay(pdMS_TO_TICKS(STREAM_DELAY_MS));
    }

    // 发送结束边界
    httpd_resp_send_chunk(req, "\r\n--" PART_BOUNDARY "--\r\n", 0);
    return res;
}
