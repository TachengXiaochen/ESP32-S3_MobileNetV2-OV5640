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

// ============================================================
// ============================================================
// 模糊检测阈值（标准拉普拉斯方差 Var(L)，2× 降采样 160×120）
// OV5640 QVGA 实测: 清晰 300-1000, 边缘模糊 50-150, 严重模糊 <50
// ============================================================

/** @brief 模糊接受阈值 — 低于此值判定为模糊，丢弃帧 */
#define BLUR_ACCEPT_THRESHOLD     50.0f

/** @brief 高置信阈值 — 高于此值帧非常清晰，单帧即可使用 */
#define BLUR_CONFIDENT_THRESHOLD  150.0f

/**
 * @brief 计算图像的拉普拉斯方差（内部使用 2× 降采样加速）
 *
 * @param img 输入图像 (RGB888格式, 320×240)
 * @return float 拉普拉斯方差值（降采样后的值），值越小表示图像越模糊
 */
float blur_detect_laplacian_variance(const image_t* img);

/**
 * @brief 判断图像是否清晰（非模糊）
 *
 * @param img 输入图像 (RGB888格式)
 * @param threshold 阈值，建议 BLUR_ACCEPT_THRESHOLD (12.5)
 * @return true 图像清晰，可以用于特征提取
 * @return false 图像模糊，应该丢弃
 */
bool blur_detect_is_sharp(const image_t* img, float threshold);

/**
 * @brief 快速判断图像是否清晰（使用默认 BLUR_ACCEPT_THRESHOLD）
 *
 * @param img 输入图像 (RGB888格式)
 * @return true 图像清晰
 * @return false 图像模糊
 */
static inline bool blur_detect_is_sharp_default(const image_t* img) {
    return blur_detect_is_sharp(img, BLUR_ACCEPT_THRESHOLD);
}

#ifdef __cplusplus
}
#endif

#endif // BLUR_DETECTION_H