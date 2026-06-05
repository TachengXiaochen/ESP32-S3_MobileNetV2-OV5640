#ifndef BUSINESS_EXECUTOR_H
#define BUSINESS_EXECUTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "modules/system/asset_manager.h"  // asset_record_t, TAG_ID_STR_LEN, FEATURE_VEC_SIZE

#ifdef __cplusplus
extern "C" {
#endif

// ========== 输出通道枚举（仅用于回调标识）==========
typedef enum {
    BE_CHANNEL_UART0_TEXT,    // UART0 CLI: 人读文本
    BE_CHANNEL_UART1_JSON     // UART1 WS63: JSON 协议
} be_channel_t;

// ========== 视图类型枚举（替代 protocol_handler.h 的 capture_view_t）==========
typedef enum {
    BE_VIEW_NONE = 0,
    BE_VIEW_FRONT,
    BE_VIEW_SIDE,
    BE_VIEW_TOP
} be_view_t;

// ========== 错误码枚举（替代 protocol_handler.h 的 ws63_error_t）==========
typedef enum {
    BE_ERR_NONE = 0,
    BE_ERR_INVALID_JSON,
    BE_ERR_UNKNOWN_CMD,
    BE_ERR_MISSING_FIELD,
    BE_ERR_INVALID_TAG_ID,
    BE_ERR_INVALID_FIELD,
    BE_ERR_ASSET_NOT_FOUND,
    BE_ERR_ASSET_ALREADY_EXISTS,
    BE_ERR_STORAGE_NOT_READY,
    BE_ERR_CAMERA_FAIL,
    BE_ERR_AI_MODEL_FAIL,
    BE_ERR_CAPTURE_FAIL,
    BE_ERR_SAVE_FAIL,
    BE_ERR_INTERNAL_ERROR,
    BE_ERR_NOT_INITIALIZED,
    BE_ERR_TASK_BUSY,
    BE_ERR_INVALID_STATE,
    BE_ERR_VERIFICATION_FAILED,
    BE_ERR_VERIFY_RETRIES_EXCEEDED,
    BE_ERR_GENERIC
} be_error_code_t;

// ========== 业务命令枚举 ==========
typedef enum {
    BE_CMD_REGISTER = 0,
    BE_CMD_INVENTORY,
    BE_CMD_OUTBOUND,
    BE_CMD_CAPTURE_FRONT,
    BE_CMD_CAPTURE_SIDE,
    BE_CMD_CAPTURE_TOP,
    BE_CMD_DELETE,
    BE_CMD_CANCEL,
    BE_CMD_LIST_ASSETS,
    BE_CMD_LIST_ASSETS_PAGE,
    BE_CMD_GET_ASSET,
    BE_CMD_SYS_INFO,
    BE_CMD_PING,
    BE_CMD_UNKNOWN
} be_cmd_t;

// ========== 业务事件枚举 ==========
typedef enum {
    BE_EVT_TASK_STARTED,        // 任务已提交
    BE_EVT_HARDWARE_READY,      // 硬件初始化完成，等待拍摄
    BE_EVT_CAPTURE_PROGRESS,    // 单个视图拍摄+推理完成
    BE_EVT_TASK_DONE,           // 全部视图完成，最终结果
    BE_EVT_ASSET_LIST_RESULT,   // 资产列表结果
    BE_EVT_ASSET_DETAIL,        // 单个资产详情
    BE_EVT_SYS_INFO_RESULT,     // 系统信息
    BE_EVT_ASSET_INFO,          // 资产信息（出库查询结果，含 remove_qty/remaining_qty）
    BE_EVT_PONG_RESULT,         // 心跳响应
    BE_EVT_ERROR                // 错误
} be_event_t;

// ========== 事件数据结构（纯数据，不包含格式化）==========

typedef struct {
    int error_code;
    const char *error_msg;
} be_error_info_t;

typedef struct {
    char tag_id[TAG_ID_STR_LEN];
    char item_name[128];
    char storage_area;
    uint32_t quantity;
    int total_views;
} be_hardware_ready_t;

typedef struct {
    char tag_id[TAG_ID_STR_LEN];
    int view_index;        // 0=front, 1=side, 2=top
    int total_steps;
    float blur_score;
} be_capture_progress_t;

typedef struct {
    be_cmd_t task;
    char tag_id[TAG_ID_STR_LEN];
    char item_name[128];
    char storage_area;
    uint32_t quantity;
    uint32_t previous_qty;     // 验证式更新用
    uint32_t remove_qty;       // 出库用
    bool is_match;
    float confidence;
    float threshold;
    bool is_verify_mode;
    bool is_overwrite;
    const char *result;          // "success" / "failed" / "cancelled"
} be_task_done_t;

typedef struct {
    int total_count;
    char json_payload[2048];   // 预序列化的 JSON（业务层不关心格式）
} be_asset_list_t;

typedef struct {
    char tag_id[TAG_ID_STR_LEN];
    bool found;
    char item_name[128];
    char storage_area;
    uint32_t quantity;
} be_asset_detail_t;

typedef struct {
    uint32_t free_heap;
    bool camera_ready;
    bool storage_ready;
    uint32_t storage_total_mb;
    uint32_t storage_free_mb;
    const char *state_str;
} be_sys_info_t;

typedef struct {
    char tag_id[TAG_ID_STR_LEN];
    char item_name[128];
    char storage_area;
    uint32_t quantity;         // 当前库存
    uint32_t remove_qty;       // 出库数量
    uint32_t remaining_qty;    // 出库后剩余
} be_asset_info_t;

typedef struct {
    bool camera_ready;
    bool storage_ready;
    uint32_t free_heap;
    const char *state_str;
} be_pong_t;

// ========== 响应回调类型 ==========

/**
 * @brief 业务事件回调函数
 *
 * 由 uart_handler_0 / uart_handler_1 注册，business_executor 在事件发生时调用。
 * 根据 channel 值，handler 选择合适的输出格式：
 *   - BE_CHANNEL_UART0_TEXT: 格式化为可读文本，写 UART0
 *   - BE_CHANNEL_UART1_JSON: 格式化为 JSON 协议，写 UART1
 *
 * @param channel  事件来源通道（由发起方在 be_execute() 时设置）
 * @param event    事件类型
 * @param data     事件数据（根据 event 类型转换）
 */
typedef void (*be_response_cb_t)(be_channel_t channel, be_event_t event, const void *data);

// ========== 公开 API ==========

/**
 * @brief 初始化 business_executor
 * @param resp_cb 响应回调函数
 *
 * @note 必须在 uart_handler_0/1 初始化之前调用
 */
void be_init(be_response_cb_t resp_cb);

/**
 * @brief 执行业务命令
 *
 * 此函数只做参数校验和状态检查，然后通过 xSystemQueue / xInferenceQueue
 * 分发到 main.c 的异步任务队列。结果通过 be_response_cb_t 回调返回。
 *
 * @param channel   来源通道
 * @param cmd       业务命令
 * @param tag_id    Tag ID（可为 NULL）
 * @param params    JSON 参数字符串（由 uart_handler_1 传来；uart_handler_0 传 NULL）
 * @return ESP_OK 已提交，ESP_FAIL 参数/状态错误
 */
esp_err_t be_execute(be_channel_t channel, be_cmd_t cmd,
                     const char *tag_id, const char *params);

/**
 * @brief 获取当前状态字符串
 */
const char *be_get_state_string(void);

/**
 * @brief 取消当前任务
 */
esp_err_t be_cancel(be_channel_t channel);

/**
 * @brief 标记一个视图拍摄完成（由 handle_capture_view 调用）
 */
void be_on_view_captured(int view_index);

/**
 * @brief 全部视图推理完成（由 handle_inference_trigger 调用）
 */
bool be_on_all_views_done(void);  // 返回 true 表示 business_executor 已处理，旧路径应跳过

/**
 * @brief 注册 ws63_send_json_raw 函数指针（用于 L610 URC 主动上报）
 *
 * uart_handler_1 在初始化时调用此函数，让 L610 manager 可以直接往 WS63 发数据。
 */
void be_register_ws63_send_func(void (*func)(const char *));

#ifdef __cplusplus
}
#endif

#endif /* BUSINESS_EXECUTOR_H */