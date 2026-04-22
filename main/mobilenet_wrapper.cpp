#include "mobilenet_wrapper.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "dl_image.hpp"
#include "imagenet_cls.hpp"
#include <cstring>
#include <map>

static const char *TAG = "mobilenet_wrapper";

// MobileNetV2模型实例
static ImageNetCls *g_mobilenet_model = nullptr;

extern "C" bool mobilenet_init(void)
{
    ESP_LOGI(TAG, "Initializing MobileNetV2 model...");
    g_mobilenet_model = new ImageNetCls(ImageNetCls::MOBILENETV2_S8_V1, false);
    if (!g_mobilenet_model) {
        ESP_LOGE(TAG, "Failed to create MobileNetV2 model");
        return false;
    }
    ESP_LOGI(TAG, "MobileNetV2 model initialized");
    return true;
}

extern "C" bool mobilenet_extract_features(float *feature_vec, int feature_size)
{
    ESP_LOGI(TAG, "Starting MobileNetV2 feature extraction...");

    // 捕获图像
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        return false;
    }

    ESP_LOGI(TAG, "Image captured: %u x %u, format: %d", fb->width, fb->height, fb->format);

    // 初始化模型（如果尚未初始化）
    if (!g_mobilenet_model) {
        ESP_LOGE(TAG, "Model not initialized");
        esp_camera_fb_return(fb);
        return false;
    }

    // 获取原始模型以访问中间层
    dl::Model *model = g_mobilenet_model->get_raw_model();
    if (!model) {
        ESP_LOGE(TAG, "Failed to get raw model");
        esp_camera_fb_return(fb);
        return false;
    }

    // 准备输入图像 (RGB565 -> RGB888)
    uint16_t *img_rgb565 = (uint16_t *)fb->buf;
    size_t num_pixels = fb->len / 2;

    // 分配RGB888缓冲区
    uint8_t *rgb888 = (uint8_t *)malloc(num_pixels * 3);
    if (!rgb888) {
        ESP_LOGE(TAG, "Failed to allocate RGB888 buffer");
        esp_camera_fb_return(fb);
        return false;
    }

    // 转换RGB565到RGB888
    for (size_t i = 0; i < num_pixels; i++) {
        uint16_t rgb565 = img_rgb565[i];
        uint8_t r = (rgb565 >> 11) & 0x1F;
        uint8_t g = (rgb565 >> 5) & 0x3F;
        uint8_t b = rgb565 & 0x1F;

        rgb888[i * 3 + 0] = (r << 3) | (r >> 2);
        rgb888[i * 3 + 1] = (g << 2) | (g >> 4);
        rgb888[i * 3 + 2] = (b << 3) | (b >> 2);
    }

    // 创建dl::image::img_t结构
    dl::image::img_t input_img;
    input_img.data = rgb888;
    input_img.width = fb->width;
    input_img.height = fb->height;
    input_img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;  // 使用正确的枚举值

    ESP_LOGI(TAG, "Running MobileNetV2 inference...");

    // 运行模型推理
    auto results = g_mobilenet_model->run(input_img);

    if (results.empty()) {
        ESP_LOGE(TAG, "Model inference failed");
        free(rgb888);
        esp_camera_fb_return(fb);
        return false;
    }

    // 获取模型的最后一层输出作为特征向量
    std::map<std::string, dl::TensorBase *> outputs = model->get_outputs();

    if (outputs.empty()) {
        ESP_LOGE(TAG, "No model outputs available");
        free(rgb888);
        esp_camera_fb_return(fb);
        return false;
    }

    // 获取第一个输出（通常是softmax之前的logits或全局平均池化后的特征）
    auto output_iter = outputs.begin();
    dl::TensorBase *output_tensor = output_iter->second;

    if (!output_tensor) {
        ESP_LOGE(TAG, "Output tensor is null");
        free(rgb888);
        esp_camera_fb_return(fb);
        return false;
    }

    int feat_len = output_tensor->get_size();
    ESP_LOGI(TAG, "Feature vector length: %d", feat_len);

    // 确保特征向量长度不超过目标大小
    if (feat_len > feature_size) {
        ESP_LOGW(TAG, "Feature length %d exceeds buffer size %d, truncating", feat_len, feature_size);
        feat_len = feature_size;
    }

    // 反量化输出并复制到特征向量
    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
        int8_t *quant_data = (int8_t *)output_tensor->get_element_ptr();
        float scale = DL_SCALE(output_tensor->exponent);

        for (int i = 0; i < feat_len; i++) {
            feature_vec[i] = quant_data[i] * scale;
        }
    } else if (output_tensor->get_dtype() == dl::DATA_TYPE_INT16) {
        int16_t *quant_data = (int16_t *)output_tensor->get_element_ptr();
        float scale = DL_SCALE(output_tensor->exponent);

        for (int i = 0; i < feat_len; i++) {
            feature_vec[i] = quant_data[i] * scale;
        }
    } else if (output_tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        float *float_data = (float *)output_tensor->get_element_ptr();
        memcpy(feature_vec, float_data, feat_len * sizeof(float));
    }

    // L2归一化特征向量（用于余弦相似度计算）
    float norm = 0.0f;
    for (int i = 0; i < feat_len; i++) {
        norm += feature_vec[i] * feature_vec[i];
    }
    norm = sqrtf(norm);

    if (norm > 1e-6f) {
        for (int i = 0; i < feat_len; i++) {
            feature_vec[i] /= norm;
        }
    }

    // 填充剩余部分为0
    for (int i = feat_len; i < feature_size; i++) {
        feature_vec[i] = 0.0f;
    }

    ESP_LOGI(TAG, "Feature extraction completed: %d features, norm=%.4f", feat_len, norm);

    free(rgb888);
    esp_camera_fb_return(fb);

    return true;
}

extern "C" void mobilenet_deinit(void)
{
    if (g_mobilenet_model) {
        delete g_mobilenet_model;
        g_mobilenet_model = nullptr;
        ESP_LOGI(TAG, "MobileNetV2 model deinitialized");
    }
}