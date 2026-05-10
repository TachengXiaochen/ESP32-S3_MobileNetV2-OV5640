#ifndef SIMILARITY_MATCHER_H
#define SIMILARITY_MATCHER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 资产类别枚举（用于动态阈值）
 */
typedef enum {
    ASSET_CLASS_UNKNOWN = 0,      // 未知类别，使用默认阈值0.75
    ASSET_CLASS_ELECTRONIC = 1,   // 电子设备，使用较高阈值0.85
    ASSET_CLASS_FURNITURE = 2,    // 家具，使用较低阈值0.70
    ASSET_CLASS_TOOL = 3,         // 工具，使用中等阈值0.78
    ASSET_CLASS_CONTAINER = 4     // 容器，使用中等阈值0.75
} asset_class_t;

/**
 * @brief 相似度匹配结果结构体
 */
typedef struct {
    float cosine_similarity;       // 余弦相似度 (0-1)
    float euclidean_similarity;    // 欧氏相似度 (0-1, 1-归一化欧氏距离)
    float mixed_similarity;        // 混合相似度 (0.7*cosine + 0.3*euclidean)
    float confidence;              // 校准后的置信度 (0-1)
    float match_threshold;         // 动态阈值
    bool is_match;                 // 是否匹配
} similarity_result_t;

/**
 * @brief 初始化相似度匹配器
 * @return true 成功，false 失败
 */
bool similarity_matcher_init(void);

/**
 * @brief 反初始化
 */
void similarity_matcher_deinit(void);

/**
 * @brief 计算两个特征向量的余弦相似度
 * @param feat1 特征向量1
 * @param feat2 特征向量2
 * @param size 特征维度
 * @return 余弦相似度 (0-1)
 */
float similarity_matcher_cosine(const float *feat1, const float *feat2, int size);

/**
 * @brief 计算两个特征向量的欧氏距离（归一化）
 * @param feat1 特征向量1
 * @param feat2 特征向量2
 * @param size 特征维度
 * @return 欧氏相似度 (0-1, 1为完全相同)
 */
float similarity_matcher_euclidean(const float *feat1, const float *feat2, int size);

/**
 * @brief 计算混合相似度（0.7*cosine + 0.3*euclidean）
 * @param feat1 特征向量1
 * @param feat2 特征向量2
 * @param size 特征维度
 * @return 混合相似度 (0-1)
 */
float similarity_matcher_mixed(const float *feat1, const float *feat2, int size);

/**
 * @brief 获取动态阈值（基于资产类别）
 * @param asset_class 资产类别
 * @return 推荐的匹配阈值
 */
float similarity_matcher_get_threshold(asset_class_t asset_class);

/**
 * @brief 置信度校准（将相似度转换为概率）
 * 基于学习的相似度-准确率映射表
 * @param similarity 输入相似度 (0-1)
 * @return 校准后的置信度 (0-1)
 */
float similarity_matcher_calibrate_confidence(float similarity);

/**
 * @brief 完整的相似度匹配流程
 * @param feat1 特征向量1
 * @param feat2 特征向量2
 * @param size 特征维度
 * @param asset_class 资产类别（用于动态阈值）
 * @param result 输出结果指针
 * @return true 成功，false 失败
 */
bool similarity_matcher_match(const float *feat1, const float *feat2, int size,
                              asset_class_t asset_class, similarity_result_t *result);

#ifdef __cplusplus
}
#endif

#endif // SIMILARITY_MATCHER_H
