#include "feature_processor.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "feature_processor";

// 默认配置
#define DEFAULT_TEMPERATURE_SCALE 0.8f
#define DEFAULT_NUM_FRAMES 3
#define DEFAULT_BATCH_NORM_MOMENTUM 0.1f

// 特征融合缓冲区管理
typedef struct {
    float **frame_buffer;          // 特征帧缓冲区
    int buffer_capacity;           // 缓冲区容量
    int frame_count;               // 当前帧数
    int feature_size;              // 特征维度
    
    // 批归一化参数
    float *channel_mean;           // 通道均值
    float *channel_var;            // 通道方差
    float batch_norm_momentum;
    
    // 配置
    feature_processor_config_t config;
} feature_processor_state_t;

static feature_processor_state_t g_processor = {0};
static bool g_initialized = false;

bool feature_processor_init(const feature_processor_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "Feature processor already initialized");
        return true;
    }

    // 使用默认或指定的配置
    if (config) {
        g_processor.config = *config;
    } else {
        g_processor.config.temperature_scale = DEFAULT_TEMPERATURE_SCALE;
        g_processor.config.num_frames = DEFAULT_NUM_FRAMES;
        g_processor.config.enable_batch_norm = true;
        g_processor.config.batch_norm_momentum = DEFAULT_BATCH_NORM_MOMENTUM;
    }

    ESP_LOGI(TAG, "Feature processor initialized with T=%.2f, frames=%d", 
             g_processor.config.temperature_scale, g_processor.config.num_frames);
    
    g_initialized = true;
    return true;
}

void feature_processor_deinit(void)
{
    if (!g_initialized) return;

    feature_processor_clear_buffer();
    
    if (g_processor.channel_mean) {
        free(g_processor.channel_mean);
        g_processor.channel_mean = NULL;
    }
    if (g_processor.channel_var) {
        free(g_processor.channel_var);
        g_processor.channel_var = NULL;
    }

    g_initialized = false;
    ESP_LOGI(TAG, "Feature processor deinitialized");
}

bool feature_processor_add_frame(const float *feature, int feature_size)
{
    if (!g_initialized || !feature) {
        ESP_LOGE(TAG, "Invalid input or processor not initialized");
        return false;
    }

    // 首次添加帧时，初始化缓冲区
    if (g_processor.frame_buffer == NULL) {
        g_processor.feature_size = feature_size;
        g_processor.buffer_capacity = g_processor.config.num_frames;
        
        // 分配缓冲区
        g_processor.frame_buffer = (float **)malloc(sizeof(float *) * g_processor.buffer_capacity);
        if (!g_processor.frame_buffer) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            return false;
        }

        for (int i = 0; i < g_processor.buffer_capacity; i++) {
            g_processor.frame_buffer[i] = (float *)malloc(sizeof(float) * feature_size);
            if (!g_processor.frame_buffer[i]) {
                ESP_LOGE(TAG, "Failed to allocate frame %d", i);
                return false;
            }
        }

        // 初始化批归一化参数
        if (g_processor.config.enable_batch_norm) {
            g_processor.channel_mean = (float *)calloc(feature_size, sizeof(float));
            g_processor.channel_var = (float *)calloc(feature_size, sizeof(float));
            
            if (!g_processor.channel_mean || !g_processor.channel_var) {
                ESP_LOGE(TAG, "Failed to allocate batch norm buffers");
                return false;
            }

            // 初始化为1（方差）
            for (int i = 0; i < feature_size; i++) {
                g_processor.channel_var[i] = 1.0f;
            }
        }

        ESP_LOGI(TAG, "Frame buffer allocated: %d frames × %d dims", 
                 g_processor.buffer_capacity, feature_size);
    }

    // 检查维度是否匹配
    if (feature_size != g_processor.feature_size) {
        ESP_LOGE(TAG, "Feature size mismatch: expected %d, got %d",
                 g_processor.feature_size, feature_size);
        return false;
    }

    // 如果缓冲区满，则滑动窗口
    if (g_processor.frame_count >= g_processor.buffer_capacity) {
        // 向前移动所有帧
        for (int i = 0; i < g_processor.buffer_capacity - 1; i++) {
            memcpy(g_processor.frame_buffer[i], g_processor.frame_buffer[i + 1],
                   sizeof(float) * feature_size);
        }
        // 新帧放在最后
        memcpy(g_processor.frame_buffer[g_processor.buffer_capacity - 1], feature,
               sizeof(float) * feature_size);
    } else {
        // 直接添加
        memcpy(g_processor.frame_buffer[g_processor.frame_count], feature,
               sizeof(float) * feature_size);
        g_processor.frame_count++;
    }

    ESP_LOGI(TAG, "Frame added: %d/%d", g_processor.frame_count, g_processor.buffer_capacity);
    return true;
}

bool feature_processor_get_fused_feature(float *output, int feature_size)
{
    if (!g_initialized || !output || g_processor.frame_count == 0) {
        ESP_LOGE(TAG, "Invalid input or no frames in buffer");
        return false;
    }

    if (feature_size != g_processor.feature_size) {
        ESP_LOGE(TAG, "Feature size mismatch");
        return false;
    }

    // 计算特征平均值
    memset(output, 0, sizeof(float) * feature_size);
    
    for (int i = 0; i < g_processor.frame_count; i++) {
        for (int j = 0; j < feature_size; j++) {
            output[j] += g_processor.frame_buffer[i][j];
        }
    }

    for (int j = 0; j < feature_size; j++) {
        output[j] /= g_processor.frame_count;
    }

    // 如果启用批归一化，应用到融合特征
    if (g_processor.config.enable_batch_norm) {
        for (int j = 0; j < feature_size; j++) {
            // 简单的批归一化：(x - mean) / sqrt(var + eps)
            float eps = 1e-5f;
            output[j] = (output[j] - g_processor.channel_mean[j]) / 
                       sqrtf(g_processor.channel_var[j] + eps);
        }
    }

    // 应用L2归一化
    float norm = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        norm += output[i] * output[i];
    }
    norm = sqrtf(norm);

    if (norm > 1e-6f) {
        for (int i = 0; i < feature_size; i++) {
            output[i] /= norm;
        }
    }

    ESP_LOGI(TAG, "Fused feature computed from %d frames", g_processor.frame_count);
    return true;
}

int feature_processor_get_frame_count(void)
{
    return g_processor.frame_count;
}

void feature_processor_clear_buffer(void)
{
    if (!g_initialized) return;

    if (g_processor.frame_buffer) {
        for (int i = 0; i < g_processor.buffer_capacity; i++) {
            if (g_processor.frame_buffer[i]) {
                free(g_processor.frame_buffer[i]);
            }
        }
        free(g_processor.frame_buffer);
        g_processor.frame_buffer = NULL;
    }

    g_processor.frame_count = 0;
    ESP_LOGI(TAG, "Frame buffer cleared");
}

bool feature_processor_normalize(const float *feature, int feature_size, float *output)
{
    if (!feature || !output || feature_size <= 0) {
        return false;
    }

    // 复制特征
    memcpy(output, feature, sizeof(float) * feature_size);

    // 计算均值和方差（通道维度）
    float mean = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        mean += output[i];
    }
    mean /= feature_size;

    float variance = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        float diff = output[i] - mean;
        variance += diff * diff;
    }
    variance /= feature_size;

    // 批归一化
    float eps = 1e-5f;
    for (int i = 0; i < feature_size; i++) {
        output[i] = (output[i] - mean) / sqrtf(variance + eps);
    }

    // L2归一化
    float norm = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        norm += output[i] * output[i];
    }
    norm = sqrtf(norm);

    if (norm > 1e-6f) {
        for (int i = 0; i < feature_size; i++) {
            output[i] /= norm;
        }
    }

    return true;
}

bool feature_processor_temperature_scaling(const float *feature, int feature_size,
                                           float temperature, float *output)
{
    if (!feature || !output || feature_size <= 0 || temperature <= 0.0f) {
        return false;
    }

    if (temperature < 0.1f || temperature > 2.0f) {
        ESP_LOGW(TAG, "Temperature %f is outside recommended range [0.1, 2.0]", temperature);
    }

    // 复制特征
    memcpy(output, feature, sizeof(float) * feature_size);

    // 计算max值用于数值稳定性
    float max_val = -1e9f;
    for (int i = 0; i < feature_size; i++) {
        if (output[i] > max_val) max_val = output[i];
    }

    // 应用温度缩放和softmax
    float sum_exp = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        output[i] = expf((output[i] - max_val) / temperature);
        sum_exp += output[i];
    }

    // 归一化为概率分布
    for (int i = 0; i < feature_size; i++) {
        output[i] /= sum_exp;
    }

    // 再次应用L2归一化（保持特征空间）
    float norm = 0.0f;
    for (int i = 0; i < feature_size; i++) {
        norm += output[i] * output[i];
    }
    norm = sqrtf(norm);

    if (norm > 1e-6f) {
        for (int i = 0; i < feature_size; i++) {
            output[i] /= norm;
        }
    }

    return true;
}
