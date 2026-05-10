#ifndef AI_MODULE_H
#define AI_MODULE_H

#include <stdbool.h>
#include "similarity_matcher.h"

/**
 * @brief 初始化 AI 模型和相关优化模块
 * @return true 成功, false 失败
 */
bool ai_module_init(void);

/**
 * @brief 反初始化 AI 模块
 */
void ai_module_deinit(void);

/**
 * @brief 计算两个特征向量之间的余弦相似度（置信度）
 * 向后兼容接口
 * @param feature1 第一个特征向量
 * @param feature2 第二个特征向量
 * @param size 特征向量维度
 * @return 置信度 (0.0 - 1.0)
 */
float ai_module_calculate_confidence(const float *feature1, const float *feature2, int size);

/**
 * @brief 计算改进的混合相似度（包含多种度量方法）
 * @param feature1 第一个特征向量
 * @param feature2 第二个特征向量
 * @param size 特征向量维度
 * @param asset_class 资产类别（用于动态阈值）
 * @param result 输出结果指针
 * @return true 成功，false 失败
 */
bool ai_module_match_features(const float *feature1, const float *feature2, int size,
                              asset_class_t asset_class, similarity_result_t *result);

#endif // AI_MODULE_H
