#ifndef AI_MODULE_H
#define AI_MODULE_H

#include <stdbool.h>

/**
 * @brief 初始化 AI 模型
 * @return true 成功, false 失败
 */
bool ai_module_init(void);

/**
 * @brief 计算两个特征向量之间的余弦相似度（置信度）
 * @param feature1 第一个特征向量
 * @param feature2 第二个特征向量
 * @param size 特征向量维度
 * @return 置信度 (0.0 - 1.0)
 */
float ai_module_calculate_confidence(const float *feature1, const float *feature2, int size);

#endif // AI_MODULE_H
