#include "ai_module.h"
#include "mobilenet_wrapper.h"
#include "esp_log.h"

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