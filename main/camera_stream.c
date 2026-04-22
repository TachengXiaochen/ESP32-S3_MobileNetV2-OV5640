#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "sdkconfig.h"
#include "camera_stream.h"

static const char *TAG = "camera_stream";

// Part boundary for multipart JPEG stream
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

// HTTP Server handle
static httpd_handle_t stream_httpd = NULL;

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index) {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[128];
    static int64_t last_frame = 0;

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // Convert to JPEG if needed
        if (fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if (!jpeg_converted) {
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
                break;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        // Send timestamp
        int64_t fr_end = esp_timer_get_time();
        int seconds = (fr_end / 1000000) % 60;
        int microseconds = fr_end % 1000000;

        // Build part header
        snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, seconds, microseconds);

        // Send boundary
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK) {
            goto done;
        }

        // Send part header
        res = httpd_resp_send_chunk(req, (const char *)part_buf, strlen((const char *)part_buf));
        if (res != ESP_OK) {
            goto done;
        }

        // Send JPEG data
        res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        if (res != ESP_OK) {
            goto done;
        }

        // Free JPEG buffer if we converted
        if (fb->format != PIXFORMAT_JPEG) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }

        // Return frame buffer
        esp_camera_fb_return(fb);
        fb = NULL;
        _jpg_buf = NULL;

        // Calculate FPS
        if (last_frame) {
            int64_t frame_time = fr_end - last_frame;
            if (frame_time > 0) {
                float fps = 1000000.0 / frame_time;
                ESP_LOGD(TAG, "FPS: %.2f", fps);
            }
        }
        last_frame = fr_end;
    }

done:
    if (fb) {
        esp_camera_fb_return(fb);
    }
    if (_jpg_buf && fb->format != PIXFORMAT_JPEG) {
        free(_jpg_buf);
    }
    return res;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html_page = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <title>ESP32-CAM Stream</title>\n"
        "  <style>\n"
        "    body { font-family: Arial; text-align: center; margin: 20px; background: #f0f0f0; }\n"
        "    h1 { color: #333; }\n"
        "    .container { max-width: 800px; margin: 0 auto; }\n"
        "    img { max-width: 100%; height: auto; border: 2px solid #333; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }\n"
        "    .info { margin-top: 20px; padding: 15px; background: white; border-radius: 8px; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <h1>ESP32-S3 Camera Live Stream</h1>\n"
        "    <img src=\"/stream\" id=\"stream\">\n"
        "    <div class=\"info\">\n"
        "      <p><strong>Status:</strong> <span id=\"status\">Loading...</span></p>\n"
        "      <p><small>Refresh page if stream doesn't load</small></p>\n"
        "    </div>\n"
        "  </div>\n"
        "  <script>\n"
        "    const img = document.getElementById('stream');\n"
        "    const status = document.getElementById('status');\n"
        "    \n"
        "    img.onload = function() {\n"
        "      status.textContent = 'Streaming';\n"
        "      status.style.color = 'green';\n"
        "    };\n"
        "    \n"
        "    img.onerror = function() {\n"
        "      status.textContent = 'Error - Retrying...';\n"
        "      status.style.color = 'red';\n"
        "      setTimeout(() => { img.src = '/stream?' + Date.now(); }, 2000);\n"
        "    };\n"
        "    \n"
        "    img.src = '/stream';\n"
        "  </script>\n"
        "</body>\n"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

httpd_handle_t start_camera_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;  // Increase stack size

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    if (httpd_start(&stream_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    // Register URI handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    if (httpd_register_uri_handler(stream_httpd, &index_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register index handler");
        httpd_stop(stream_httpd);
        return NULL;
    }

    if (httpd_register_uri_handler(stream_httpd, &stream_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register stream handler");
        httpd_stop(stream_httpd);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "Open http://<ESP32-IP>/ in browser to view live stream");
    
    return stream_httpd;
}

void stop_camera_stream_server(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}