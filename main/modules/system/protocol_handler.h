#ifndef PROTOCOL_HANDLER_H
#define PROTOCOL_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== 命令类型枚举 ==========
typedef enum {
    CMD_REGISTER = 0,      // 入库注册
    CMD_INVENTORY,         // 盘点比对
    CMD_OUTBOUND,          // 出库核验
    CMD_CAPTURE,           // 单步拍摄视图
    CMD_DELETE,            // 删除资产
    CMD_CANCEL,            // 取消当前任务
    CMD_LIST_ASSETS,       // 查询资产列表（已弃用，请使用 CMD_LIST_ASSETS_PAGE）
    CMD_LIST_ASSETS_PAGE,  // ⭐ 分页查询资产列表
    CMD_GET_ASSET,         // 查询单个资产详情
    CMD_SYS_INFO,          // 查询系统信息
    CMD_PING,              // 心跳/状态检测

    // ===== L610 4G 相关命令 (v2.0) =====
    CMD_MQTT_CONNECT,      // 连接MQTT代理 (协议§13.2.1)
    CMD_MQTT_DISCONNECT,   // 断开MQTT连接 (协议§13.2.1)
    CMD_MQTT_PUBLISH,      // 通过4G发布MQTT消息 (协议§13.2.2)
    CMD_L610_STATUS,       // 查询4G模块状态 (协议§13.2.3)
    CMD_L610_AT,           // AT指令透传调试 (协议§13.2.4)
    CMD_L610_MQTT_CHECK,   // 检查4G MQTT连接状态 (内部扩展)

    CMD_UNKNOWN            // 未知命令
} ws63_cmd_t;

// ========== 错误码枚举 ==========
typedef enum {
    ERR_INVALID_JSON = 0,
    ERR_UNKNOWN_CMD,
    ERR_MISSING_FIELD,
    ERR_INVALID_MAC,
    ERR_INVALID_FIELD,
    ERR_ASSET_NOT_FOUND,
    ERR_ASSET_ALREADY_EXISTS,
    ERR_STORAGE_NOT_READY,
    ERR_CAMERA_FAIL,
    ERR_AI_MODEL_FAIL,
    ERR_CAPTURE_FAIL,
    ERR_BLUR_DETECTED,
    ERR_INFERENCE_FAIL,
    ERR_SAVE_FAIL,
    ERR_INTERNAL_ERROR,
    ERR_NOT_INITIALIZED,
    ERR_TASK_BUSY,
    ERR_INVALID_STATE,      // 无效状态错误
    // 新增错误码（与文档对齐）
    ERR_INVALID_ARG,        // 通用参数错误
    ERR_CAMERA_INIT,        // 摄像头初始化失败
    ERR_SD_CARD,            // SD卡错误
    ERR_INSUFFICIENT_SPACE, // 存储空间不足
    ERR_LOW_CONFIDENCE,     // 置信度过低
    ERR_TIMEOUT,            // 超时
    // ⭐ Tag ID 改造新增错误码
    ERR_INVALID_TAG_ID,     // Tag ID格式无效
    ERR_VERIFICATION_FAILED, // 验证失败（物品不匹配）
    ERR_VERIFY_RETRIES_EXCEEDED // 验证重试次数超过上限
} ws63_error_t;

// ========== 视图类型枚举 ==========
typedef enum {
    VIEW_NONE = 0,
    VIEW_FRONT,
    VIEW_SIDE,
    VIEW_TOP
} capture_view_t;

// ========== 资产类别枚举（已在similarity_matcher.h中定义）==========
// 注意：asset_class_t 已在 similarity_matcher.h 中完整定义
// 此处不重复定义，使用时需包含 similarity_matcher.h 或 ai_module.h

// ========== 函数原型 ==========

/**
 * @brief 初始化协议处理器
 */
void protocol_handler_init(void);

/**
 * @brief 处理接收到的JSON命令
 * @param json_str JSON字符串
 * @return ESP_OK 处理成功，其他为错误码
 */
esp_err_t protocol_handle_command(const char *json_str);

/**
 * @brief WS63 UART接收任务（前向声明）
 */
void ws63_recv_task(void *pvParameters);

/**
 * @brief 发送JSON响应到WS63
 * @param json_str JSON字符串
 * @return ESP_OK 发送成功，其他为错误码
 */
esp_err_t protocol_send_response(const char *json_str);

/**
 * @brief 生成错误响应JSON
 * @param error_code 错误码
 * @param error_msg 错误消息
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_error_response(ws63_error_t error_code, const char *error_msg, 
                                          char *json_buf, size_t buf_size);

/**
 * @brief 生成拍摄进度响应JSON
 * @param mac MAC地址
 * @param view 视图类型
 * @param step 步骤字符串（如"1/3"）
 * @param status 状态（"ok"|"failed"|"blur_warning"）
 * @param blur_score 模糊度得分
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_capture_progress(const char *tag_id, capture_view_t view, 
                                            const char *step, const char *status,
                                            float blur_score, char *json_buf, size_t buf_size);

/**
 * @brief 生成任务完成响应JSON
 * @param task 任务类型
 * @param mac MAC地址
 * @param result 结果（"success"|"failed"|"cancelled"）
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_task_done(ws63_cmd_t task, const char *tag_id, const char *result,
                                     char *json_buf, size_t buf_size);

/**
 * @brief 生成资产列表响应JSON
 * @param count 资产数量
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_asset_list(int count, char *json_buf, size_t buf_size);

/**
 * @brief 生成系统信息响应JSON
 * @param free_heap 可用堆内存
 * @param min_free_heap 最小可用堆内存
 * @param camera_ready 摄像头就绪状态
 * @param storage_ready 存储就绪状态
 * @param storage_total_mb 存储总容量
 * @param storage_free_mb 存储剩余容量
 * @param current_task 当前任务状态
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_sys_info(uint32_t free_heap, uint32_t min_free_heap,
                                    bool camera_ready, bool storage_ready,
                                    uint32_t storage_total_mb, uint32_t storage_free_mb,
                                    const char *current_task, char *json_buf, size_t buf_size);

/**
 * @brief 生成心跳响应JSON
 * @param camera_ready 摄像头就绪状态
 * @param storage_ready 存储就绪状态
 * @param free_heap 可用堆内存
 * @param current_task 当前任务状态
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return ESP_OK 生成成功，其他为错误码
 */
esp_err_t protocol_generate_pong(bool camera_ready, bool storage_ready,
                                uint32_t free_heap, const char *current_task,
                                char *json_buf, size_t buf_size);

/**
 * @brief 获取错误码对应的字符串描述
 * @param error_code 错误码
 * @return 错误描述字符串
 */
const char *protocol_get_error_string(ws63_error_t error_code);

/**
 * @brief 获取命令类型对应的字符串
 * @param cmd 命令类型
 * @return 命令字符串
 */
const char *protocol_get_cmd_string(ws63_cmd_t cmd);

/**
 * @brief 获取视图类型对应的字符串
 * @param view 视图类型
 * @return 视图字符串
 */
const char *protocol_get_view_string(capture_view_t view);

// ===== L610 4G 辅助接口 (v2.0) =====

/**
 * @brief 注册L610发送回调函数 (由4G模块初始化时调用)
 * 
 * l610_manager通过此函数向protocol_handler注入"往WS63发数据"的能力,
 * 避免循环依赖.
 * 
 * @param send_func 与protocol_send_response同类型的函数指针
 */
void protocol_register_l610_send_cb(void (*send_func)(const char *));

/**
 * @brief 检查当前是否可以通过4G发布 (即4G模块就绪)
 * @return true 就绪, false 未就绪
 */
bool protocol_is_l610_ready(void);

/**
 * @brief 获取当前4G/L610模块状态JSON字符串
 * @param json_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t protocol_get_l610_status_json(char *json_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_HANDLER_H */
