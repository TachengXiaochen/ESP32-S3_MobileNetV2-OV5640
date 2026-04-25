#ifndef CAMERA_MODULE_H
#define CAMERA_MODULE_H

#include <stdbool.h>

/**
 * @brief 初始化摄像头硬件
 * @return true 成功, false 失败
 */
bool camera_module_init(void);

/**
 * @brief 采集图像并提取特征向量
 * @param feature_out 输出特征向量缓冲区
 * @param feature_size 特征向量大小
 * @return true 成功, false 失败
 */
bool camera_module_capture_and_process(float *feature_out, int feature_size);

/**
 * @brief 反初始化摄像头硬件（关闭摄像头）
 */
void camera_module_deinit(void);

#endif // CAMERA_MODULE_H
