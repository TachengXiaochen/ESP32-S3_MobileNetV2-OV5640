#ifndef MOBILENET_WRAPPER_H
#define MOBILENET_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化MobileNetV2模型
 * @return 成功返回true，失败返回false
 */
bool mobilenet_init(void);

/**
 * @brief 从图像中提取MobileNetV2特征
 * @param feature_vec 输出特征向量
 * @param feature_size 特征向量大小
 * @param blur_score 可选输出，返回当前帧的Laplacian模糊方差（传NULL忽略）
 * @return 成功返回true，失败返回false
 */
bool mobilenet_extract_features(float *feature_vec, int feature_size, float *blur_score);

/**
 * @brief 从已抓取的帧中提取特征（不调用 esp_camera_fb_get/return）
 * @param fb 已抓取的帧缓冲（调用者负责 fb_return）
 * @param feature_vec 输出特征向量
 * @param feature_size 特征向量大小
 * @param blur_score 可选输出模糊分数
 */
bool mobilenet_extract_features_from_frame(void *fb, float *feature_vec, int feature_size, float *blur_score);

/**
 * @brief 释放MobileNetV2模型资源
 */
void mobilenet_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // MOBILENET_WRAPPER_H