#ifndef FEATURE_PROCESSOR_H
#define FEATURE_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 特征处理配置结构体
 */
typedef struct {
    float temperature_scale;      // 温度参数 T (通常 0.6-1.0，值越小区分度越高)
    int num_frames;               // 融合的帧数 (建议3-5)
    bool enable_batch_norm;       // 是否启用批归一化
    float batch_norm_momentum;    // 批归一化动量
} feature_processor_config_t;

/**
 * @brief 初始化特征处理器
 * @param config 配置参数指针（如果为NULL，使用默认配置）
 * @return true 成功，false 失败
 */
bool feature_processor_init(const feature_processor_config_t *config);

/**
 * @brief 反初始化特征处理器
 */
void feature_processor_deinit(void);

/**
 * @brief 添加特征到融合缓冲区
 * @param feature 输入特征向量指针
 * @param feature_size 特征向量维度
 * @return true 成功，false 失败
 */
bool feature_processor_add_frame(const float *feature, int feature_size);

/**
 * @brief 获取融合后的特征向量（平均值）
 * @param output 输出特征向量指针
 * @param feature_size 特征向量维度
 * @return true 成功（融合完整），false 失败或融合不足
 */
bool feature_processor_get_fused_feature(float *output, int feature_size);

/**
 * @brief 获取当前缓冲区中的帧数
 * @return 缓冲区中的帧数
 */
int feature_processor_get_frame_count(void);

/**
 * @brief 清空融合缓冲区
 */
void feature_processor_clear_buffer(void);

/**
 * @brief 强化的特征归一化（包含通道维度的批归一化）
 * @param feature 输入特征向量
 * @param feature_size 特征向量维度
 * @param output 输出特征向量
 * @return true 成功，false 失败
 */
bool feature_processor_normalize(const float *feature, int feature_size, float *output);

/**
 * @brief 应用温度缩放
 * 对softmax输出进行缩放，增强区分度
 * @param feature 输入特征向量
 * @param feature_size 特征向量维度
 * @param temperature 温度参数（0.1-2.0，越小越锐利）
 * @param output 输出特征向量
 * @return true 成功，false 失败
 */
bool feature_processor_temperature_scaling(const float *feature, int feature_size, 
                                           float temperature, float *output);

#ifdef __cplusplus
}
#endif

#endif // FEATURE_PROCESSOR_H
