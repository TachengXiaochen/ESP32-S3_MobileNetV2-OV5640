#include "web_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "web_server";

// SoftAP 配置
#define WIFI_SSID       "ESP32-CAM-AI"
#define WIFI_PASSWORD   "12345678"
#define WIFI_MAX_CONN   4
#define WIFI_CHANNEL    1

static httpd_handle_t g_httpd = NULL;
static bool g_wifi_ready = false;
static int g_sta_count = 0;

// WiFi 事件信号量
static SemaphoreHandle_t g_wifi_sem = NULL;

// ---------- 前向声明 ----------
static esp_err_t mount_spiffs(void);
static esp_err_t start_wifi_softap(void);
static esp_err_t start_http_server(void);

// 外部 URI 处理函数（在 mjpeg_handler.c 和 api_handlers.c 中定义）
extern esp_err_t stream_handler(httpd_req_t *req);
extern esp_err_t api_status_handler(httpd_req_t *req);
extern esp_err_t api_snapshot_handler(httpd_req_t *req);
extern esp_err_t api_frames_handler(httpd_req_t *req);
extern esp_err_t api_image_handler(httpd_req_t *req);
extern esp_err_t api_assets_handler(httpd_req_t *req);
extern esp_err_t spiffs_index_handler(httpd_req_t *req);
extern esp_err_t spiffs_css_handler(httpd_req_t *req);
extern esp_err_t spiffs_js_handler(httpd_req_t *req);

// 供 api_handlers.c 查询 WiFi 客户端数
int web_get_sta_count(void) { return g_sta_count; }

// ========== WiFi 事件处理 ==========
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "WiFi SoftAP started");
            g_wifi_ready = true;
            xSemaphoreGive(g_wifi_sem);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            g_sta_count++;
            ESP_LOGI(TAG, "Station connected (total: %d)", g_sta_count);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (g_sta_count > 0) g_sta_count--;
            ESP_LOGI(TAG, "Station disconnected (total: %d)", g_sta_count);
        }
    }
}

// ========== SPIFFS 挂载 ==========
static esp_err_t mount_spiffs(void)
{
    ESP_LOGI(TAG, "Mounting SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u/%u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

// ========== WiFi SoftAP 初始化 ==========
static esp_err_t start_wifi_softap(void)
{
    ESP_LOGI(TAG, "Initializing WiFi SoftAP...");

    g_wifi_sem = xSemaphoreCreateBinary();
    if (!g_wifi_sem) {
        ESP_LOGE(TAG, "Failed to create WiFi semaphore");
        return ESP_ERR_NO_MEM;
    }

    // 初始化 TCP/IP 栈和事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    // 注册 WiFi 事件
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL, NULL));

    // WiFi 初始化 —— 使用完全默认配置，不覆盖任何参数
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = 0,
            .password = "",
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,   // 先验证射频是否正常，再恢复加密
            .channel = WIFI_CHANNEL,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 等待 AP 启动（最多 10 秒）
    if (xSemaphoreTake(g_wifi_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "WiFi SoftAP start timeout!");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "WiFi SoftAP ready: SSID=%s, IP=192.168.4.1", WIFI_SSID);
    return ESP_OK;
}

// ========== HTTP 服务器注册 URI ==========
static void register_uri_handlers(httpd_handle_t server)
{
    // 静态文件
    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = spiffs_index_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_index);

    httpd_uri_t uri_css = {
        .uri = "/style.css", .method = HTTP_GET, .handler = spiffs_css_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_css);

    httpd_uri_t uri_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = spiffs_js_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_js);

    // MJPEG 流
    httpd_uri_t uri_stream = {
        .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_stream);

    // API 端点
    httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_status);

    httpd_uri_t uri_snapshot = {
        .uri = "/api/snapshot", .method = HTTP_GET, .handler = api_snapshot_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_snapshot);

    httpd_uri_t uri_frames = {
        .uri = "/api/frames", .method = HTTP_GET, .handler = api_frames_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_frames);

    httpd_uri_t uri_image = {
        .uri = "/api/image", .method = HTTP_GET, .handler = api_image_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_image);

    httpd_uri_t uri_assets = {
        .uri = "/api/assets", .method = HTTP_GET, .handler = api_assets_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_assets);
}

// ========== 启动 HTTP 服务器 ==========
static esp_err_t start_http_server(void)
{
    ESP_LOGI(TAG, "Starting HTTP server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.task_priority = 5;
    config.server_port = 80;
    config.max_open_sockets = 10;     // 默认7不够（MJPEG流+CSS+JS+API并发）
    config.lru_purge_enable = true;   // 连接池满时自动清理最久未用的连接

    esp_err_t ret = httpd_start(&g_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    register_uri_handlers(g_httpd);
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

// ========== 公共 API ==========
esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "========== Web Server Init (delayed) ==========");

    // 步骤 1: 挂载 SPIFFS
    esp_err_t ret = mount_spiffs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed, web UI unavailable but continuing");
        // 不阻塞启动 —— 即使没有前端文件，API 和 MJPEG 仍可用
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // 步骤 2: WiFi SoftAP
    ret = start_wifi_softap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi SoftAP failed, web server unavailable");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    // 步骤 3: HTTP 服务器
    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "========== Web Server Ready at http://192.168.4.1 ==========");
    return ESP_OK;
}

void web_server_deinit(void)
{
    if (g_httpd) {
        httpd_stop(g_httpd);
        g_httpd = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_vfs_spiffs_unregister("storage");
    ESP_LOGI(TAG, "Web server deinitialized");
}
