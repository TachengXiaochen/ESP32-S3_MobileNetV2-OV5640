/**
 * @file blur_detection.h
 * @brief 模糊度检测模块 - 使用拉普拉斯方差检测图像模糊度
 * 
 * 该模块提供图像模糊度检测功能，主要用于在特征提取前过滤模糊图像，
 * 防止低质量特征污染数据库，提升匹配准确率。
 */

#ifndef BLUR_DETECTION_H
#define BLUR_DETECTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 图像结构体
 */
typedef struct {
    uint8_t* data;      ///< 图像数据指针 (RGB888格式)
    int width;          ///< 图像宽度
    int height;         ///< 图像高度
    int channels;       ///< 通道数 (3 for RGB888)
} image_t;

/**
 * @brief 计算图像的拉普拉斯方差
 * 
 * @param img 输入图像 (RGB888格式)
 * @return float 拉普拉斯方差值，值越小表示图像越模糊
 */
float blur_detect_laplacian_variance(const image_t* img);

/**
 * @brief 判断图像是否清晰（非模糊）
 * 
 * @param img 输入图像 (RGB888格式)
 * @param threshold 阈值，默认建议50.0
 * @return true 图像清晰，可以用于特征提取
 * @return false 图像模糊，应该丢弃
 */
bool blur_detect_is_sharp(const image_t* img, float threshold);

/**
 * @brief 快速判断图像是否清晰（使用默认阈值50.0）
 * 
 * @param img 输入图像 (RGB888格式)
 * @return true 图像清晰
 * @return false 图像模糊
 */
static inline bool blur_detect_is_sharp_default(const image_t* img) {
    return blur_detect_is_sharp(img, 50.0f);
}

#ifdef __cplusplus
}
#endif

#endif // BLUR_DETECTION_H