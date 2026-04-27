#include "mobilenet_wrapper.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_task_wdt.h"  // 看门狗复位函数
#include "dl_image.hpp"
#include "imagenet_cls.hpp"
#include "feature_processor.h"  // 温度缩放功能
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

    // 【关键修复】增加摄像头捕获重试机制
    camera_fb_t *fb = nullptr;
    int retry_count = 0;
    const int MAX_RETRIES = 3;
    
    while (retry_count < MAX_RETRIES) {
        fb = esp_camera_fb_get();
        if (fb) {
            break; // 成功获取帧
        }
        
        retry_count++;
        ESP_LOGW(TAG, "Camera capture failed (attempt %d/%d), retrying...", retry_count, MAX_RETRIES);
        
        // 延迟后重试,让摄像头有时间恢复
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed after %d retries", MAX_RETRIES);
        return false;
    }

    ESP_LOGI(TAG, "Image captured: %u x %u, format: %d, size: %u bytes", 
             fb->width, fb->height, fb->format, (unsigned int)fb->len);

    // ✅ 验证格式必须为JPEG
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Invalid pixel format: %d (expected JPEG)", fb->format);
        esp_camera_fb_return(fb);
        return false;
    }

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

    // ✅ 使用ESP-DL的JPEG解码功能，直接将JPEG解码为RGB888
    dl::image::jpeg_img_t jpeg_img;
    jpeg_img.data = fb->buf;
    jpeg_img.data_len = fb->len;
    
    ESP_LOGI(TAG, "Decoding JPEG to RGB888...");
    
    // 软件解码JPEG到RGB888
    dl::image::img_t input_img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    
    if (!input_img.data) {
        ESP_LOGE(TAG, "JPEG decoding failed");
        esp_camera_fb_return(fb);
        return false;
    }
    
    ESP_LOGI(TAG, "JPEG decoded successfully: %dx%d", input_img.width, input_img.height);

    ESP_LOGI(TAG, "Running MobileNetV2 inference...");

    // 运行模型推理
    auto results = g_mobilenet_model->run(input_img);

    if (results.empty()) {
        ESP_LOGE(TAG, "Model inference failed");
        // ✅ 释放JPEG解码内存
        free(input_img.data);
        esp_camera_fb_return(fb);
        return false;
    }

    // 获取模型的最后一层输出作为特征向量
    std::map<std::string, dl::TensorBase *> outputs = model->get_outputs();

    if (outputs.empty()) {
        ESP_LOGE(TAG, "No model outputs available");
        // ✅ 释放JPEG解码内存
        free(input_img.data);
        esp_camera_fb_return(fb);
        return false;
    }

    // 获取第一个输出（通常是softmax之前的logits或全局平均池化后的特征）
    auto output_iter = outputs.begin();
    dl::TensorBase *output_tensor = output_iter->second;

    if (!output_tensor) {
        ESP_LOGE(TAG, "Output tensor is null");
        // ✅ 释放JPEG解码内存
        free(input_img.data);
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

    // ✅ 反量化完成后复位看门狗
    esp_task_wdt_reset();

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

    // ✅ 新增：应用温度缩放增强区分度 (T=0.8)
    extern bool feature_processor_temperature_scaling(const float *feature, int feature_size,
                                                       float temperature, float *output);
    float *scaled_features = (float *)malloc(feat_len * sizeof(float));
    if (scaled_features && feature_processor_temperature_scaling(feature_vec, feat_len, 0.8f, scaled_features)) {
        memcpy(feature_vec, scaled_features, sizeof(float) * feat_len);
        ESP_LOGI(TAG, "Temperature scaling applied (T=0.8)");
    } else {
        ESP_LOGW(TAG, "Temperature scaling failed, using original features");
    }
    if (scaled_features) {
        free(scaled_features);
    }

    // ✅ 归一化完成后复位看门狗
    esp_task_wdt_reset();

    // 填充剩余部分为0
    for (int i = feat_len; i < feature_size; i++) {
        feature_vec[i] = 0.0f;
    }

    ESP_LOGI(TAG, "Feature extraction completed: %d features, norm=%.4f", feat_len, norm);

    // ✅ 关键修复：释放JPEG解码分配的内存
    if (input_img.data) {
        free(input_img.data);
        input_img.data = nullptr;
    }

    // 清除模型输出引用，防止悬空指针
    outputs.clear();
    
    esp_camera_fb_return(fb);
    
    // 【关键修复】强制触发一次堆管理器整理
    // 通过分配和释放小块内存，检测并修复可能的堆损坏
    void *heap_check = malloc(128);
    if (heap_check) {
        memset(heap_check, 0, 128);
        free(heap_check);
    }
    
    // 【关键修复】增加延迟时间,确保堆管理器完全稳定
    // MobileNetV2推理后需要更长时间让TLSF堆元数据恢复
    // 但在延迟期间必须定期复位看门狗,防止系统重启
    
    // 分段延迟,每200ms复位一次看门狗
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_task_wdt_reset();  // 每200ms复位看门狗
    }

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