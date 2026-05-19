#ifndef VERIFY_CONFIG_H
#define VERIFY_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 验证配置参数
 * 
 * 所有关键参数集中在此文件中定义，便于统一管理和调整。
 * 未来可迁移到 menuconfig 实现运行时配置。
 */

// ========== 相似度验证阈值 ==========

/**
 * @brief 默认验证阈值
 * 
 * 混合相似度（0.7×余弦 + 0.3×欧氏）≥ 此阈值时认为匹配。
 * 取值范围：0.0 - 1.0
 * 推荐值：0.75（默认），0.85（严格），0.65（宽松）
 */
#define VERIFY_THRESHOLD_DEFAULT    0.75f

/**
 * @brief 高精度模式阈值（用于外观相似的物品）
 */
#define VERIFY_THRESHOLD_HIGH       0.85f

/**
 * @brief 宽松模式阈值（允许一定角度和光照变化）
 */
#define VERIFY_THRESHOLD_LOW        0.65f

// ========== 验证模式枚举 ==========

typedef enum {
    VERIFY_MODE_FAST = 0,       // 快速模式：仅正视图（默认）
    VERIFY_MODE_STANDARD = 1,   // 标准模式：正视图 + 侧视图
    VERIFY_MODE_STRICT = 2      // 严格模式：三视图全部比对
} verify_mode_t;

// ========== 验证参数配置 ==========

/**
 * @brief 当前使用的验证模式
 */
#define VERIFY_MODE_DEFAULT     VERIFY_MODE_FAST

/**
 * @brief 验证所需视图（默认仅正视图）
 * 
 * 0 = VIEW_FRONT
 * 1 = VIEW_SIDE
 * 2 = VIEW_TOP
 */
#define VERIFY_REQUIRED_VIEW    0   // VIEW_FRONT

/**
 * @brief 最大重试次数
 * 
 * 验证失败后允许用户重新摆放物品并重试的次数。
 * 超过此次数后锁定并要求人工介入。
 */
#define VERIFY_RETRIES_MAX      3

/**
 * @brief 余弦相似度权重（混合相似度计算用）
 */
#define VERIFY_COSINE_WEIGHT    0.70f

/**
 * @brief 欧氏距离权重（混合相似度计算用）
 */
#define VERIFY_EUCLIDEAN_WEIGHT 0.30f

/**
 * @brief 验证结果枚举
 */
typedef enum {
    VERIFY_RESULT_MATCH = 0,        // 验证通过：相似度 ≥ 阈值
    VERIFY_RESULT_LOW_CONF = 1,     // 低置信度：阈值-0.1 ≤ 相似度 < 阈值（可二次验证）
    VERIFY_RESULT_MISMATCH = 2,     // 验证失败：相似度 < 阈值-0.1
    VERIFY_RESULT_ERROR = 3         // 验证过程出错（特征向量损坏等）
} verify_result_t;

#ifdef __cplusplus
}
#endif

#endif /* VERIFY_CONFIG_H */