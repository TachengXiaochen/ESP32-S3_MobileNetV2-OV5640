#include "mobilenet_wrapper.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_task_wdt.h"  // 看门狗复位函数
#include "dl_image.hpp"
#include "dl_cls_base.hpp"
#include "dl_tensor_base.hpp"
#include "imagenet_cls.hpp"
#include "fbs_model.hpp"
#include "feature_processor.h"  // 温度缩放功能
#include "blur_detection.h"    // 模糊度检测
#include <cstring>
#include <map>

static const char *TAG = "mobilenet_wrapper";

// MobileNetV2模型实例
static ImageNetCls *g_mobilenet_model = nullptr;

// GAP（Global Average Pooling）中间层 — 1280 维语义特征
static std::string g_gap_tensor_name;
static std::vector<int> g_gap_shape;
static int g_gap_module_index = -1;  // -1 = 未发现，>=0 = 在 m_execution_plan 中的索引

/**
 * @brief 遍历模型节点，找到 GlobalAveragePool 操作的输出张量
 */
static bool discover_gap_tensor(dl::Model *model)
{
    auto *fbs = model->get_fbs_model();
    fbs->load_map();  // build() 后映射表已被 clear_map() 清空，需重建
    auto nodes = fbs->topological_sort();
    bool found = false;
    for (int i = 0; i < (int)nodes.size(); i++) {
        if (fbs->get_operation_type(nodes[i]) == "GlobalAveragePool") {
            std::vector<std::string> inputs, outputs;
            fbs->get_operation_inputs_and_outputs(nodes[i], inputs, outputs);
            if (!outputs.empty()) {
                g_gap_tensor_name = outputs[0];
                g_gap_module_index = i;  // 拓扑排序索引 = 执行计划索引
                g_gap_shape = fbs->get_tensor_shape(g_gap_tensor_name);
                if (g_gap_shape.empty()) {
                    // 中间张量不在 FlatBuffers initializers 中，使用标准 MobileNetV2 GAP 输出
                    g_gap_shape = {1, 1280};
                }
                ESP_LOGI(TAG, "GAP tensor discovered: module_idx=%d, tensor=%s, size=%d",
                         g_gap_module_index, g_gap_tensor_name.c_str(),
                         g_gap_shape.empty() ? 0 : g_gap_shape.back());
                found = true;
            }
            break;
        }
    }
    fbs->clear_map();  // 释放映射表内存
    if (!found) {
        ESP_LOGW(TAG, "GlobalAveragePool layer not found, falling back to graph output");
    }
    return found;
}

extern "C" bool mobilenet_init(void)
{
    ESP_LOGI(TAG, "Initializing MobileNetV2 model...");
    g_mobilenet_model = new ImageNetCls(ImageNetCls::MOBILENETV2_S8_V1, false);
    if (!g_mobilenet_model) {
        ESP_LOGE(TAG, "Failed to create MobileNetV2 model");
        return false;
    }

    // 发现 GAP 中间层（1280 维语义特征，优于 1000 维分类 logits）
    dl::Model *model = g_mobilenet_model->get_raw_model();
    if (model) {
        discover_gap_tensor(model);
    }

    ESP_LOGI(TAG, "MobileNetV2 model initialized");
    return true;
}

// ========== 内部：特征提取核心（不管理帧缓冲区生命周期）==========
// fb 必须已被调用者抓取。调用者负责 esp_camera_fb_return(fb)。
static bool extract_features_core(camera_fb_t *fb, float *feature_vec,
                                  int feature_size, float *blur_score)
{
    ESP_LOGI(TAG, "Image captured: %u x %u, format: %d, size: %u bytes",
             fb->width, fb->height, fb->format, (unsigned int)fb->len);

    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Invalid pixel format: %d (expected JPEG)", fb->format);
        return false;
    }
    if (!g_mobilenet_model) {
        ESP_LOGE(TAG, "Model not initialized");
        return false;
    }
    dl::Model *model = g_mobilenet_model->get_raw_model();
    if (!model) {
        ESP_LOGE(TAG, "Failed to get raw model");
        return false;
    }

    dl::image::jpeg_img_t jpeg_img;
    jpeg_img.data = fb->buf;
    jpeg_img.data_len = fb->len;
    ESP_LOGI(TAG, "Decoding JPEG to RGB888...");
    dl::image::img_t input_img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!input_img.data) {
        ESP_LOGE(TAG, "JPEG decoding failed");
        return false;
    }
    ESP_LOGI(TAG, "JPEG decoded successfully: %dx%d", input_img.width, input_img.height);

    ESP_LOGI(TAG, "Checking image sharpness...");
    image_t img_for_blur_detect = {
        .data = static_cast<uint8_t*>(input_img.data),
        .width = input_img.width, .height = input_img.height, .channels = 3
    };
    float variance = blur_detect_laplacian_variance(&img_for_blur_detect);
    if (blur_score) *blur_score = variance;
    ESP_LOGI(TAG, "Blur variance: %.2f", (double)variance);

    if (!blur_detect_is_sharp_default(&img_for_blur_detect)) {
        ESP_LOGW(TAG, "Image is too blurry, discarding frame");
        free(input_img.data);
        return false;
    }
    ESP_LOGI(TAG, "Image is sharp enough for feature extraction");
    ESP_LOGI(TAG, "Running MobileNetV2 inference...");

    dl::TensorBase *output_tensor = nullptr;
    int feat_len = 0;

    if (g_gap_module_index >= 0) {
        auto *preprocessor = g_mobilenet_model->get_image_preprocessor();
        preprocessor->preprocess(input_img);
        model->run_until(g_gap_module_index);
        dl::TensorBase *gap_tensor = model->get_intermediate(g_gap_tensor_name);
        if (!gap_tensor) {
            ESP_LOGE(TAG, "Failed to get GAP intermediate tensor");
            free(input_img.data);
            return false;
        }
        output_tensor = gap_tensor;
        feat_len = output_tensor->get_size();
        ESP_LOGI(TAG, "GAP DIAG: dtype=%d exponent=%d size=%d data=%p",
                 (int)output_tensor->get_dtype(), output_tensor->get_exponent(),
                 output_tensor->get_size(), output_tensor->get_element_ptr());
        if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
            int8_t *raw = (int8_t *)output_tensor->get_element_ptr();
            ESP_LOGI(TAG, "GAP DIAG raw[0..9]: %d %d %d %d %d %d %d %d %d %d",
                     raw[0], raw[1], raw[2], raw[3], raw[4],
                     raw[5], raw[6], raw[7], raw[8], raw[9]);
        }
        ESP_LOGI(TAG, "Feature vector length (GAP): %d", feat_len);
        model->run_from(g_gap_module_index);
    } else {
        auto results = g_mobilenet_model->run(input_img);
        if (results.empty()) {
            ESP_LOGE(TAG, "Model inference failed");
            free(input_img.data);
            return false;
        }
        std::map<std::string, dl::TensorBase *> outputs = model->get_outputs();
        if (outputs.empty()) {
            ESP_LOGE(TAG, "No model outputs available");
            free(input_img.data);
            return false;
        }
        output_tensor = outputs.begin()->second;
        if (!output_tensor) {
            ESP_LOGE(TAG, "Output tensor is null");
            free(input_img.data);
            return false;
        }
        feat_len = output_tensor->get_size();
        ESP_LOGI(TAG, "Feature vector length (logits): %d", feat_len);
        outputs.clear();
    }

    if (feat_len > feature_size) {
        ESP_LOGW(TAG, "Feature length %d exceeds buffer size %d, truncating", feat_len, feature_size);
        feat_len = feature_size;
    }

    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
        int8_t *quant_data = (int8_t *)output_tensor->get_element_ptr();
        float scale = DL_SCALE(output_tensor->exponent);
        for (int i = 0; i < feat_len; i++) feature_vec[i] = quant_data[i] * scale;
    } else if (output_tensor->get_dtype() == dl::DATA_TYPE_INT16) {
        int16_t *quant_data = (int16_t *)output_tensor->get_element_ptr();
        float scale = DL_SCALE(output_tensor->exponent);
        for (int i = 0; i < feat_len; i++) feature_vec[i] = quant_data[i] * scale;
    } else if (output_tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        float *float_data = (float *)output_tensor->get_element_ptr();
        memcpy(feature_vec, float_data, feat_len * sizeof(float));
    }

    esp_task_wdt_reset();
    for (int i = feat_len; i < feature_size; i++) feature_vec[i] = 0.0f;
    ESP_LOGI(TAG, "Feature extraction completed: %d features", feat_len);

    if (input_img.data) { free(input_img.data); input_img.data = nullptr; }

    void *heap_check = malloc(128);
    if (heap_check) { memset(heap_check, 0, 128); free(heap_check); }
    vTaskDelay(1);
    return true;
}

// ========== 公开 API：自行抓取帧并提取特征（原接口，向后兼容）==========
extern "C" bool mobilenet_extract_features(float *feature_vec, int feature_size, float *blur_score)
{
    ESP_LOGI(TAG, "Starting MobileNetV2 feature extraction...");

    camera_fb_t *fb = nullptr;
    int retry_count = 0;
    const int MAX_RETRIES = 3;
    while (retry_count < MAX_RETRIES) {
        fb = esp_camera_fb_get();
        if (fb) break;
        retry_count++;
        ESP_LOGW(TAG, "Camera capture failed (attempt %d/%d), retrying...", retry_count, MAX_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed after %d retries", MAX_RETRIES);
        return false;
    }

    bool result = extract_features_core(fb, feature_vec, feature_size, blur_score);
    esp_camera_fb_return(fb);
    return result;
}

// ========== 公开 API：从已抓取帧中提取特征（新接口，调用者管理帧生命周期）==========
extern "C" bool mobilenet_extract_features_from_frame(void *fb_ptr, float *feature_vec,
                                                       int feature_size, float *blur_score)
{
    return extract_features_core((camera_fb_t *)fb_ptr, feature_vec, feature_size, blur_score);
}

extern "C" void mobilenet_deinit(void)
{
    if (g_mobilenet_model) {
        delete g_mobilenet_model;
        g_mobilenet_model = nullptr;
        ESP_LOGI(TAG, "MobileNetV2 model deinitialized");
    }
}