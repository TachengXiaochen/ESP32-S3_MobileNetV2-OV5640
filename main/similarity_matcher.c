#include "similarity_matcher.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "similarity_matcher";

// 动态阈值定义
#define THRESHOLD_ELECTRONIC  0.85f
#define THRESHOLD_FURNITURE   0.70f
#define THRESHOLD_TOOL        0.78f
#define THRESHOLD_CONTAINER   0.75f
#define THRESHOLD_DEFAULT     0.75f

// 置信度校准参数（基于学习的映射表）
// 这些参数是通过历史数据拟合得到的
typedef struct {
    float similarity;    // 相似度输入
    float confidence;    // 校准后的置信度
} calibration_point_t;

// 简化的校准查找表（可从训练数据中学习）
static const calibration_point_t g_calibration_table[] = {
    {0.50f, 0.01f},  // 相似度0.5 -> 置信度1%
    {0.60f, 0.10f},
    {0.65f, 0.25f},
    {0.70f, 0.50f},
    {0.75f, 0.70f},
    {0.80f, 0.85f},
    {0.85f, 0.92f},
    {0.90f, 0.97f},
    {0.95f, 0.99f},
    {1.00f, 1.00f}
};
static const int g_calibration_table_size = sizeof(g_calibration_table) / sizeof(calibration_point_t);

static bool g_initialized = false;

bool similarity_matcher_init(void)
{
    if (g_initialized) return true;
    
    ESP_LOGI(TAG, "Similarity matcher initialized");
    g_initialized = true;
    return true;
}

void similarity_matcher_deinit(void)
{
    g_initialized = false;
}

float similarity_matcher_cosine(const float *feat1, const float *feat2, int size)
{
    if (!feat1 || !feat2 || size <= 0) {
        return 0.0f;
    }

    float dot_product = 0.0f;
    float norm1 = 0.0f;
    float norm2 = 0.0f;

    for (int i = 0; i < size; i++) {
        dot_product += feat1[i] * feat2[i];
        norm1 += feat1[i] * feat1[i];
        norm2 += feat2[i] * feat2[i];
    }

    norm1 = sqrtf(norm1);
    norm2 = sqrtf(norm2);

    if (norm1 < 1e-6f || norm2 < 1e-6f) {
        return 0.0f;
    }

    float cosine = dot_product / (norm1 * norm2);

    // 余弦相似度范围: [-1, 1] -> [0, 1]
    float similarity = (cosine + 1.0f) / 2.0f;

    // 限制在[0, 1]范围
    if (similarity < 0.0f) similarity = 0.0f;
    if (similarity > 1.0f) similarity = 1.0f;

    return similarity;
}

float similarity_matcher_euclidean(const float *feat1, const float *feat2, int size)
{
    if (!feat1 || !feat2 || size <= 0) {
        return 0.0f;
    }

    float sum_squared_diff = 0.0f;
    for (int i = 0; i < size; i++) {
        float diff = feat1[i] - feat2[i];
        sum_squared_diff += diff * diff;
    }

    float euclidean_distance = sqrtf(sum_squared_diff);

    // 特征通常已归一化到[0,1]范围或[-1,1]范围
    // 最大欧氏距离约为 sqrt(2)*size（对于[-1,1]范围的特征）
    // 为了得到[0,1]的相似度，我们使用：similarity = 1 / (1 + distance)
    float similarity = 1.0f / (1.0f + euclidean_distance / (float)size);

    if (similarity < 0.0f) similarity = 0.0f;
    if (similarity > 1.0f) similarity = 1.0f;

    return similarity;
}

float similarity_matcher_mixed(const float *feat1, const float *feat2, int size)
{
    if (!feat1 || !feat2 || size <= 0) {
        return 0.0f;
    }

    float cosine_sim = similarity_matcher_cosine(feat1, feat2, size);
    float euclidean_sim = similarity_matcher_euclidean(feat1, feat2, size);

    // 混合相似度：0.7*cosine + 0.3*euclidean
    float mixed = 0.7f * cosine_sim + 0.3f * euclidean_sim;

    return mixed;
}

float similarity_matcher_get_threshold(asset_class_t asset_class)
{
    switch (asset_class) {
        case ASSET_CLASS_ELECTRONIC:
            return THRESHOLD_ELECTRONIC;
        case ASSET_CLASS_FURNITURE:
            return THRESHOLD_FURNITURE;
        case ASSET_CLASS_TOOL:
            return THRESHOLD_TOOL;
        case ASSET_CLASS_CONTAINER:
            return THRESHOLD_CONTAINER;
        case ASSET_CLASS_UNKNOWN:
        default:
            return THRESHOLD_DEFAULT;
    }
}

float similarity_matcher_calibrate_confidence(float similarity)
{
    // 使用线性插值进行置信度校准
    if (similarity < 0.0f) similarity = 0.0f;
    if (similarity > 1.0f) similarity = 1.0f;

    // 找到相应的区间
    for (int i = 0; i < g_calibration_table_size - 1; i++) {
        float sim1 = g_calibration_table[i].similarity;
        float conf1 = g_calibration_table[i].confidence;
        float sim2 = g_calibration_table[i + 1].similarity;
        float conf2 = g_calibration_table[i + 1].confidence;

        if (similarity >= sim1 && similarity <= sim2) {
            // 线性插值
            float t = (similarity - sim1) / (sim2 - sim1);
            float confidence = conf1 + t * (conf2 - conf1);
            return confidence;
        }
    }

    // 超出范围，返回边界值
    if (similarity < g_calibration_table[0].similarity) {
        return g_calibration_table[0].confidence;
    } else {
        return g_calibration_table[g_calibration_table_size - 1].confidence;
    }
}

bool similarity_matcher_match(const float *feat1, const float *feat2, int size,
                              asset_class_t asset_class, similarity_result_t *result)
{
    if (!feat1 || !feat2 || !result || size <= 0 || !g_initialized) {
        return false;
    }

    // 计算各种相似度
    result->cosine_similarity = similarity_matcher_cosine(feat1, feat2, size);
    result->euclidean_similarity = similarity_matcher_euclidean(feat1, feat2, size);
    result->mixed_similarity = similarity_matcher_mixed(feat1, feat2, size);

    // 使用混合相似度进行校准
    result->confidence = similarity_matcher_calibrate_confidence(result->mixed_similarity);

    // 获取动态阈值
    result->match_threshold = similarity_matcher_get_threshold(asset_class);

    // 判断是否匹配（使用混合相似度）
    result->is_match = (result->mixed_similarity >= result->match_threshold);

    ESP_LOGI(TAG, "Similarity match: cosine=%.4f, euclidean=%.4f, mixed=%.4f, confidence=%.4f, threshold=%.2f, match=%s",
             result->cosine_similarity, result->euclidean_similarity, result->mixed_similarity,
             result->confidence, result->match_threshold, result->is_match ? "YES" : "NO");

    return true;
}
