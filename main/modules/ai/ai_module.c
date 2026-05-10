#include "ai_module.h"
#include "mobilenet_wrapper.h"
#include "similarity_matcher.h"
#include "feature_processor.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "ai_mod";
static bool g_model_loaded = false;

bool ai_module_init(void) {
    if (g_model_loaded) return true;

    ESP_LOGI(TAG, "Loading MobileNetV2 model...");
    if (!mobilenet_init()) {
        ESP_LOGE(TAG, "Failed to initialize MobileNet");
        return false;
    }
    
    ESP_LOGI(TAG, "Initializing similarity matcher...");
    if (!similarity_matcher_init()) {
        ESP_LOGE(TAG, "Failed to initialize similarity matcher");
        return false;
    }
    
    ESP_LOGI(TAG, "Initializing feature processor...");
    if (!feature_processor_init(NULL)) {
        ESP_LOGE(TAG, "Failed to initialize feature processor");
        return false;
    }
    
    g_model_loaded = true;
    ESP_LOGI(TAG, "AI module initialized successfully");
    return true;
}

void ai_module_deinit(void) {
    if (!g_model_loaded) return;
    
    feature_processor_deinit();
    similarity_matcher_deinit();
    mobilenet_deinit();
    
    g_model_loaded = false;
    ESP_LOGI(TAG, "AI module deinitialized");
}

float ai_module_calculate_confidence(const float *feature1, const float *feature2, int size)
{
    if (!feature1 || !feature2 || size <= 0) {
        return 0.0f;
    }
    
    // 使用优化的混合相似度而不是简单的余弦相似度
    float similarity = similarity_matcher_mixed(feature1, feature2, size);
    
    // 校准置信度
    float confidence = similarity_matcher_calibrate_confidence(similarity);
    
    return confidence;
}

bool ai_module_match_features(const float *feature1, const float *feature2, int size,
                              asset_class_t asset_class, similarity_result_t *result)
{
    if (!g_model_loaded || !feature1 || !feature2 || !result || size <= 0) {
        ESP_LOGE(TAG, "Invalid input or module not initialized");
        return false;
    }
    
    return similarity_matcher_match(feature1, feature2, size, asset_class, result);
}
