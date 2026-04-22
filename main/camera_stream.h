#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include "esp_http_server.h"

/**
 * @brief 初始化摄像头流服务
 * @return httpd_handle_t HTTP服务器句柄，失败返回NULL
 */
httpd_handle_t start_camera_stream_server(void);

/**
 * @brief 停止摄像头流服务
 * @param server HTTP服务器句柄
 */
void stop_camera_stream_server(httpd_handle_t server);

#endif // CAMERA_STREAM_H
