#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "verify_handler.h"
#include "../ai/similarity_matcher.h"
#include "../ai/ai_module.h"

static const char *TAG = "verify_handler";

static verify_context_t g_verify_ctx;
static bool g_initialized = false;

/**
 * @brief 计算混合相似度
 * 
 * 使用余弦相似度和欧氏距离的加权平均：
 *    mixed_sim = COSINE_WEIGHT × cos_sim + EUCLIDEAN_WEIGHT × (1 - norm_euclidean)
 * 
 * @param feat1 特征向量1
 * @param feat2 特征向量2
 * @param size  向量维度
 * @return      混合相似度（0.0 - 1.0）
 */
static float calculate_mixed_similarity(const float *feat1, const float *feat2, int size)
{
    if (feat1 == NULL || feat2 == NULL || size <= 0) {
        return 0.0f;
    }
    
    // 计算余弦相似度
    float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
    for (int i = 0; i < size; i++) {
        dot += feat1[i] * feat2[i];
        norm1 += feat1[i] * feat1[i];
        norm2 += feat2[i] * feat2[i];
    }
    
    float cosine_sim = 0.0f;
    if (norm1 > 0.0f && norm2 > 0.0f) {
        cosine_sim = dot / (sqrtf(norm1) * sqrtf(norm2));
        // 限制在 [0, 1] 范围
        if (cosine_sim < 0.0f) cosine_sim = 0.0f;
        if (cosine_sim > 1.0f) cosine_sim = 1.0f;
    }
    
    // 计算归一化欧氏距离相似度
    float euclidean_dist = 0.0f;
    for (int i = 0; i < size; i++) {
        float diff = feat1[i] - feat2[i];
        euclidean_dist += diff * diff;
    }
    euclidean_dist = sqrtf(euclidean_dist / size);
    // 归一化到 [0, 1]，越小越相似
    float euclidean_sim = 1.0f - tanhf(euclidean_dist);
    if (euclidean_sim < 0.0f) euclidean_sim = 0.0f;
    
    // 混合相似度
    float mixed_sim = VERIFY_COSINE_WEIGHT * cosine_sim + 
                      VERIFY_EUCLIDEAN_WEIGHT * euclidean_sim;
    
    ESP_LOGD(TAG, "Similarity: cos=%.4f, eucl=%.4f, mixed=%.4f", 
             cosine_sim, euclidean_sim, mixed_sim);
    
    return mixed_sim;
}

/**
 * @brief 初始化验证处理器
 */
void verify_handler_init(void)
{
    memset(&g_verify_ctx, 0, sizeof(g_verify_ctx));
    g_initialized = true;
    ESP_LOGI(TAG, "Verify handler initialized");
}

/**
 * @brief 开始验证流程
 */
bool verify_handler_start(const char *tag_id, 
                          const asset_record_t *record,
                          verify_mode_t mode,
                          verify_context_t *ctx_out)
{
    if (!g_initialized) {
        ESP_LOGE(TAG, "Verify handler not initialized");
        return false;
    }
    
    if (tag_id == NULL || record == NULL || ctx_out == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for verify_start");
        return false;
    }
    
    if (g_verify_ctx.is_active) {
        ESP_LOGW(TAG, "Verification already in progress");
        return false;
    }
    
    // 填充上下文
    memset(ctx_out, 0, sizeof(verify_context_t));
    strncpy(ctx_out->tag_id, tag_id, TAG_ID_STR_LEN - 1);
    ctx_out->tag_id[TAG_ID_STR_LEN - 1] = '\0';
    ctx_out->verify_mode = mode;
    ctx_out->existing_record = (asset_record_t *)record;
    ctx_out->retry_count = 0;
    ctx_out->is_active = true;
    
    // 根据模式选择阈值
    switch (mode) {
        case VERIFY_MODE_FAST:
        default:
            ctx_out->threshold = VERIFY_THRESHOLD_DEFAULT;
            break;
        case VERIFY_MODE_STANDARD:
            ctx_out->threshold = VERIFY_THRESHOLD_HIGH;
            break;
        case VERIFY_MODE_STRICT:
            ctx_out->threshold = VERIFY_THRESHOLD_HIGH;
            break;
    }
    
    // 保存到全局上下文
    memcpy(&g_verify_ctx, ctx_out, sizeof(verify_context_t));
    
    ESP_LOGI(TAG, "Verification started: tag_id=%s, mode=%d, threshold=%.2f",
             tag_id, mode, ctx_out->threshold);
    
    return true;
}

/**
 * @brief 执行验证
 */
bool verify_handler_execute(verify_context_t *ctx,
                            const float *current_feat,
                            int view_type,
                            verify_output_t *output)
{
    if (!g_initialized || ctx == NULL || current_feat == NULL || output == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for verify_execute");
        return false;
    }
    
    if (!ctx->is_active || ctx->existing_record == NULL) {
        ESP_LOGE(TAG, "No active verification context");
        output->result = VERIFY_RESULT_ERROR;
        snprintf(output->error_msg, sizeof(output->error_msg), "No active verification");
        return false;
    }
    
    memset(output, 0, sizeof(verify_output_t));
    
    // 选择参考特征向量
    const float *reference_feat = NULL;
    switch (view_type) {
        case 0: // VIEW_FRONT
            reference_feat = ctx->existing_record->front_feature;
            break;
        case 1: // VIEW_SIDE
            reference_feat = ctx->existing_record->side_feature;
            break;
        case 2: // VIEW_TOP
            reference_feat = ctx->existing_record->top_feature;
            break;
        default:
            output->result = VERIFY_RESULT_ERROR;
            snprintf(output->error_msg, sizeof(output->error_msg), "Invalid view type: %d", view_type);
            return false;
    }
    
    if (reference_feat == NULL) {
        output->result = VERIFY_RESULT_ERROR;
        snprintf(output->error_msg, sizeof(output->error_msg), "Reference feature is NULL");
        return false;
    }
    
    // 计算混合相似度
    float mixed_sim = calculate_mixed_similarity(reference_feat, current_feat, FEATURE_VEC_SIZE);
    
    ctx->last_confidence = mixed_sim;
    output->confidence = mixed_sim;
    output->threshold_used = ctx->threshold;
    
    // 阈值判断
    if (mixed_sim >= ctx->threshold) {
        output->result = VERIFY_RESULT_MATCH;
        ESP_LOGI(TAG, "Verification MATCH: confidence=%.4f >= threshold=%.2f", 
                 mixed_sim, ctx->threshold);
    } else if (mixed_sim >= (ctx->threshold - 0.10f)) {
        output->result = VERIFY_RESULT_LOW_CONF;
        snprintf(output->error_msg, sizeof(output->error_msg),
                 "Low confidence: %.4f (threshold-0.1=%.2f, threshold=%.2f)",
                 mixed_sim, ctx->threshold - 0.10f, ctx->threshold);
        ESP_LOGW(TAG, "Verification LOW CONFIDENCE: %.4f", mixed_sim);
    } else {
        output->result = VERIFY_RESULT_MISMATCH;
        snprintf(output->error_msg, sizeof(output->error_msg),
                 "Mismatch: %.4f < threshold-0.1=%.2f",
                 mixed_sim, ctx->threshold - 0.10f);
        ESP_LOGW(TAG, "Verification MISMATCH: %.4f", mixed_sim);
    }
    
    // 更新重试计数
    ctx->retry_count++;
    
    return true;
}

/**
 * @brief 重置验证上下文
 */
void verify_handler_reset(verify_context_t *ctx)
{
    if (ctx == NULL) {
        // 重置全局上下文
        memset(&g_verify_ctx, 0, sizeof(g_verify_ctx));
        ESP_LOGI(TAG, "Global verify context reset");
        return;
    }
    
    memset(ctx, 0, sizeof(verify_context_t));
    ESP_LOGD(TAG, "Verify context reset");
}

/**
 * @brief 获取当前验证上下文的活跃状态
 */
bool verify_handler_is_active(const verify_context_t *ctx)
{
    if (ctx == NULL) {
        return g_verify_ctx.is_active;
    }
    return ctx->is_active;
}

/**
 * @brief 获取是否达到最大重试次数
 */
bool verify_handler_is_max_retries_reached(const verify_context_t *ctx)
{
    if (ctx == NULL) {
        return g_verify_ctx.retry_count >= VERIFY_RETRIES_MAX;
    }
    return ctx->retry_count >= VERIFY_RETRIES_MAX;
}