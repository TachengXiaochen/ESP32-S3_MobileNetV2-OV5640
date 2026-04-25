#include "ai_module.h"
#include "mobilenet_wrapper.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "ai_mod";
static bool g_model_loaded = false;

bool ai_module_init(void) {
    if (g_model_loaded) return true;

    ESP_LOGI(TAG, "Loading MobileNetV2 model...");
    if (mobilenet_init()) {
        g_model_loaded = true;
        ESP_LOGI(TAG, "Model loaded successfully.");
        return true;
    }
    return false;
}

float ai_module_calculate_confidence(const float *feature1, const float *feature2, int size)
{
    if (!feature1 || !feature2 || size <= 0) {
        return 0.0f;
    }
    
    // 计算余弦相似度: cos(theta) = (A·B) / (||A|| * ||B||)
    float dot_product = 0.0f;
    float norm1 = 0.0f;
    float norm2 = 0.0f;
    
    for (int i = 0; i < size; i++) {
        dot_product += feature1[i] * feature2[i];
        norm1 += feature1[i] * feature1[i];
        norm2 += feature2[i] * feature2[i];
    }
    
    norm1 = sqrtf(norm1);
    norm2 = sqrtf(norm2);
    
    // 避免除零
    if (norm1 < 1e-6f || norm2 < 1e-6f) {
        return 0.0f;
    }
    
    float cosine_similarity = dot_product / (norm1 * norm2);
    
    // 将余弦相似度映射到 [0, 1] 范围
    // cos(theta) ∈ [-1, 1] -> confidence ∈ [0, 1]
    float confidence = (cosine_similarity + 1.0f) / 2.0f;
    
    // 限制在 [0, 1] 范围内
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    
    return confidence;
}
