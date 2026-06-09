#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera_module.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "api";

// 外部引用
extern SemaphoreHandle_t xCameraMutex;
extern bool g_camera_ready;
extern bool g_storage_ready;
extern char g_current_tag_id[7];
extern const char *be_get_state_string(void);
extern int web_get_sta_count(void);

// ---- 内部辅助 ----

/**
 * @brief 从 SPIFFS 提供文件
 */
static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *filename,
                                    const char *content_type)
{
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "SPIFFS file not found: %s", path);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(fsize);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    fread(buf, 1, fsize, f);
    fclose(f);

    httpd_resp_set_type(req, content_type);
    httpd_resp_send(req, (const char *)buf, fsize);
    free(buf);
    return ESP_OK;
}

/**
 * @brief 检查是否为 Tag ID 格式（以 "0x" 开头）
 */
static bool is_tag_id_format(const char *id)
{
    if (!id) return false;
    return (strncmp(id, "0x", 2) == 0 || strncmp(id, "0X", 2) == 0);
}

// ---- SPIFFS 文件处理器 ----

esp_err_t spiffs_index_handler(httpd_req_t *req) {
    return serve_spiffs_file(req, "index.html", "text/html; charset=utf-8");
}
esp_err_t spiffs_css_handler(httpd_req_t *req) {
    return serve_spiffs_file(req, "style.css", "text/css");
}
esp_err_t spiffs_js_handler(httpd_req_t *req) {
    return serve_spiffs_file(req, "app.js", "application/javascript");
}

// ---- API 处理器 ----

/**
 * GET /api/status — 系统状态 JSON
 */
esp_err_t api_status_handler(httpd_req_t *req)
{
    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"free_heap\":%lu,"
        "\"min_free_heap\":%lu,"
        "\"camera_ready\":%s,"
        "\"storage_ready\":%s,"
        "\"be_state\":\"%s\","
        "\"current_tag_id\":\"%s\","
        "\"wifi_clients\":%d"
        "}",
        esp_get_free_heap_size(),
        esp_get_minimum_free_heap_size(),
        g_camera_ready ? "true" : "false",
        g_storage_ready ? "true" : "false",
        be_get_state_string(),
        (g_current_tag_id[0]) ? g_current_tag_id : "",
        web_get_sta_count()
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/**
 * GET /api/snapshot — 单帧 JPEG 快照
 */
esp_err_t api_snapshot_handler(httpd_req_t *req)
{
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;

    if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        bool ok = camera_module_capture_jpeg(&jpeg_buf, &jpeg_len);
        xSemaphoreGive(xCameraMutex);

        if (ok && jpeg_buf && jpeg_len > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);
            free(jpeg_buf);
            return ESP_OK;
        }
    }

    httpd_resp_send_500(req);
    return ESP_FAIL;
}

/**
 * GET /api/frames?tag_id=0x0001 — 列出指定资产的三视图信息
 */
esp_err_t api_frames_handler(httpd_req_t *req)
{
    // 解析 tag_id 查询参数
    char tag_id[16] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char *query = malloc(query_len);
    if (!query) { httpd_resp_send_500(req); return ESP_ERR_NO_MEM; }

    if (httpd_req_get_url_query_str(req, query, query_len) != ESP_OK) {
        free(query);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "tag_id", tag_id, sizeof(tag_id)) != ESP_OK) {
        free(query);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(query);

    // 验证 tag_id 格式
    if (!is_tag_id_format(tag_id) && tag_id[0] == '\0') {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // 构建三视图路径并检查
    const char *views[] = {"front", "side", "top"};
    char frame_entries[768] = {0};
    bool first = true;

    // 构建安全名称
    char safe_name[64];
    strncpy(safe_name, tag_id, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';

    // 如果不是 tag_id 格式，转换 ':' 为 '_'
    if (!is_tag_id_format(tag_id)) {
        for (int i = 0; safe_name[i]; i++) {
            if (safe_name[i] == ':') safe_name[i] = '_';
        }
    }

    for (int i = 0; i < 3; i++) {
        char img_path[160];
        // 尝试 Tag ID 子目录格式，然后旧平铺格式
        if (is_tag_id_format(tag_id)) {
            snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s/%s_%s.jpg",
                     safe_name, safe_name, views[i]);
        } else {
            snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s_%s.jpg",
                     safe_name, views[i]);
        }

        struct stat st;
        long fsize = 0;
        if (stat(img_path, &st) == 0) {
            fsize = (long)st.st_size;
        }

        // 如果新路径不存在，尝试旧路径（向后兼容）
        if (fsize == 0 && is_tag_id_format(tag_id)) {
            snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s_%s.jpg",
                     safe_name, views[i]);
            if (stat(img_path, &st) == 0) {
                fsize = (long)st.st_size;
            }
        }

        char entry[256];
        snprintf(entry, sizeof(entry),
            "%s{\"view\":\"%s\","
            "\"url\":\"/api/image?tag_id=%s&view=%s\","
            "\"size\":%ld}",
            first ? "" : ",",
            views[i], tag_id, views[i], fsize);
        strcat(frame_entries, entry);
        first = false;
    }

    char json[1024];
    snprintf(json, sizeof(json),
        "{\"tag_id\":\"%s\",\"frames\":[%s]}", tag_id, frame_entries);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/**
 * GET /api/image?tag_id=0x0001&view=front — 提供指定 JPEG 图片
 */
esp_err_t api_image_handler(httpd_req_t *req)
{
    char tag_id[16] = {0}, view[16] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1) { httpd_resp_send_404(req); return ESP_FAIL; }

    char *query = malloc(query_len);
    if (!query) { httpd_resp_send_500(req); return ESP_ERR_NO_MEM; }

    if (httpd_req_get_url_query_str(req, query, query_len) != ESP_OK ||
        httpd_query_key_value(query, "tag_id", tag_id, sizeof(tag_id)) != ESP_OK ||
        httpd_query_key_value(query, "view", view, sizeof(view)) != ESP_OK) {
        free(query);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(query);

    // 验证 view 参数
    if (strcmp(view, "front") != 0 && strcmp(view, "side") != 0 && strcmp(view, "top") != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // 构建安全名称
    char safe_name[64];
    strncpy(safe_name, tag_id, sizeof(safe_name) - 1);
    safe_name[sizeof(safe_name) - 1] = '\0';
    bool is_tag = is_tag_id_format(tag_id);
    if (!is_tag) {
        for (int i = 0; safe_name[i]; i++) {
            if (safe_name[i] == ':') safe_name[i] = '_';
        }
    }

    // 构建路径（优先 Tag ID 子目录格式）
    char img_path[160];
    if (is_tag) {
        snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s/%s_%s.jpg",
                 safe_name, safe_name, view);
    } else {
        snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s_%s.jpg",
                 safe_name, view);
    }

    FILE *f = fopen(img_path, "rb");
    if (!f) {
        // 尝试旧路径（向后兼容）
        if (is_tag) {
            snprintf(img_path, sizeof(img_path), "/sdcard/assets/%s_%s.jpg",
                     safe_name, view);
            f = fopen(img_path, "rb");
        }
        if (!f) {
            ESP_LOGW(TAG, "Image not found: %s", img_path);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) { fclose(f); httpd_resp_send_404(req); return ESP_FAIL; }

    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); httpd_resp_send_500(req); return ESP_ERR_NO_MEM; }

    fread(buf, 1, fsize, f);
    fclose(f);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, (const char *)buf, fsize);
    free(buf);
    return ESP_OK;
}

/**
 * GET /api/assets — 列出所有已注册资产
 */
esp_err_t api_assets_handler(httpd_req_t *req)
{
    char json_buf[4096];
    int offset = 0;

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "{\"assets\":[");

    DIR *dir = opendir("/sdcard/assets");
    if (!dir) {
        // SD 卡未挂载或目录不存在
        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, json_buf, offset);
        return ESP_OK;
    }

    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        // 尝试作为子目录（Tag ID 格式）
        char sub_path[320];
        snprintf(sub_path, sizeof(sub_path), "/sdcard/assets/%s", entry->d_name);
        DIR *sub_dir = opendir(sub_path);
        if (sub_dir) {
            // Tag ID 子目录
            struct dirent *sub_entry;
            while ((sub_entry = readdir(sub_dir)) != NULL) {
                if (strstr(sub_entry->d_name, ".dat")) {
                    char tag_id[16] = {0};
                    strncpy(tag_id, sub_entry->d_name, sizeof(tag_id) - 1);
                    char *dot = strstr(tag_id, ".dat");
                    if (dot) *dot = '\0';

                    if (!first) json_buf[offset++] = ',';
                    first = false;
                    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                        "{\"tag_id\":\"%s\"}", tag_id);

                    if (offset >= (int)sizeof(json_buf) - 64) break;
                }
            }
            closedir(sub_dir);
        } else if (strstr(entry->d_name, ".dat")) {
            // 旧格式平铺 .dat 文件
            char tag_id[64] = {0};
            strncpy(tag_id, entry->d_name, sizeof(tag_id) - 1);
            char *dot = strstr(tag_id, ".dat");
            if (dot) *dot = '\0';

            if (!first) json_buf[offset++] = ',';
            first = false;
            offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                "{\"tag_id\":\"%s\"}", tag_id);
        }

        if (offset >= (int)sizeof(json_buf) - 64) break;
    }
    closedir(dir);

    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_buf, offset);
    return ESP_OK;
}
