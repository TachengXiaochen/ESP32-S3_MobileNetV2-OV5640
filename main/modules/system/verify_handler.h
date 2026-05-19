#ifndef VERIFY_HANDLER_H
#define VERIFY_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "asset_manager.h"
#include "verify_config.h"
#include "tag_id_validator.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 验证上下文结构
 * 
 * 保存验证流程中的状态和中间数据，
 * 支持验证重试机制。
 */
typedef struct {
    uint16_t retry_count;               // 当前已重试次数
    verify_mode_t verify_mode;          // 验证模式
    float threshold;                    // 使用的阈值
    float last_confidence;              // 最近一次验证的置信度
    char tag_id[TAG_ID_STR_LEN];        // 待验证的 Tag ID
    asset_record_t *existing_record;    // 已存在的资产记录指针
    bool is_active;                     // 是否有验证在进行中
} verify_context_t;

/**
 * @brief 验证结果详细结构
 */
typedef struct {
    verify_result_t result;             // 验证结果
    float confidence;                   // 置信度（0.0 - 1.0）
    float threshold_used;               // 实际使用的阈值
    char error_msg[128];                // 错误描述（验证失败时）
} verify_output_t;

/**
 * @brief 初始化验证处理器
 * 
 * 在系统启动时调用一次。
 */
void verify_handler_init(void);

/**
 * @brief 开始验证流程
 * 
 * @param tag_id    待验证的 Tag ID
 * @param record    已存在的资产记录
 * @param mode      验证模式
 * @param ctx_out   输出参数，验证上下文
 * @return true     验证流程启动成功
 * @return false    参数无效或已有验证在进行
 */
bool verify_handler_start(const char *tag_id, 
                          const asset_record_t *record,
                          verify_mode_t mode,
                          verify_context_t *ctx_out);

/**
 * @brief 执行验证
 * 
 * 使用当前拍摄的特征向量与参考特征向量进行比对。
 * 
 * @param ctx           验证上下文
 * @param current_feat  当前拍摄的特征向量
 * @param view_type     视图类型（0=front, 1=side, 2=top）
 * @param output        输出参数，验证结果
 * @return true         验证执行成功（结果在 output 中）
 * @return false        验证过程出错（特征向量损坏等）
 */
bool verify_handler_execute(verify_context_t *ctx,
                            const float *current_feat,
                            int view_type,
                            verify_output_t *output);

/**
 * @brief 重置验证上下文
 * 
 * 验证完成或取消后调用。
 * 
 * @param ctx 验证上下文
 */
void verify_handler_reset(verify_context_t *ctx);

/**
 * @brief 获取当前验证上下文的活跃状态
 * 
 * @param ctx 验证上下文
 * @return true  有验证在进行
 * @return false 无活跃验证
 */
bool verify_handler_is_active(const verify_context_t *ctx);

/**
 * @brief 获取是否达到最大重试次数
 * 
 * @param ctx 验证上下文
 * @return true  已达到最大重试次数
 * @return false 还可继续重试
 */
bool verify_handler_is_max_retries_reached(const verify_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VERIFY_HANDLER_H */