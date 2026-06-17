#ifndef MAIN_H
#define MAIN_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"  // GPIO引脚定义
#include "modules/system/storage/asset_manager.h"
#include "modules/system/executor/business_executor.h"   // ⭐ 新增：业务执行器

// ========== WS63 协议配置 ==========
// UART1 使用空闲 GPIO47/21，避免与摄像头 DVP GPIO17/18 冲突
#define WS63_UART_NUM           UART_NUM_1
#define WS63_UART_TX_PIN        GPIO_NUM_47
#define WS63_UART_RX_PIN        GPIO_NUM_21
#define WS63_UART_BAUD_RATE     115200
#define WS63_UART_BUF_SIZE      1024
#define WS63_UART_QUEUE_SIZE    10

// WS63 协议缓冲区大小
#define WS63_JSON_BUF_SIZE      2048
#define WS63_TAG_ID_LEN         6   // "0xFFFF" = 6字符
#define WS63_TAG_ID_STR_LEN     7   // 含终止符

// WS63 任务配置
#define WS63_TASK_STACK_SIZE    4096
#define WS63_TASK_PRIORITY      5

// WS63 协议超时配置（单位：毫秒）
#define WS63_RESPONSE_TIMEOUT   5000
#define WS63_CAPTURE_TIMEOUT    10000

#ifdef __cplusplus
extern "C" {
#endif

// 命令枚举（需要被cmd_handler访问）
typedef enum {
    CMD_INIT_CAMERA,
    CMD_CAPTURE_FRONT,
    CMD_CAPTURE_SIDE,
    CMD_CAPTURE_TOP,
    CMD_SAVE_ASSET,
    CMD_INIT_STORAGE,
    CMD_START_INVENTORY,
    CMD_INVENTORY_WITH_MAC,
    CMD_OUTBOUND_ANALYZE,
    CMD_OUTBOUND_UPDATE_QTY,
    CMD_INFERENCE_TRIGGER,  // 推理任务触发：全部视图推理完成后触发最终操作
    CMD_DEINIT_CAMERA       // 异步反初始化摄像头（通过队列，在 camera_ai_task 中执行）
} system_cmd_t;

// 消息结构体（需要被cmd_handler访问）
typedef struct {
    system_cmd_t cmd;
    void *data;
    char tag_id[TAG_ID_STR_LEN];  // ⭐ Tag ID替代MAC地址
} system_msg_t;

// 摄像头状态枚举 ⭐ 改用Tag ID相关命名
typedef enum {
    CAM_STATE_WAITING_TAG_ID = 0,     // 主菜单状态，等待模式选择 ⭐
    CAM_STATE_WAITING_REG_TAG_ID = 1, // 等待注册Tag ID ⭐
    CAM_STATE_WAITING_REG_NAME = 2,   // 等待输入物品名称
    CAM_STATE_WAITING_REG_AREA = 3,   // 等待输入存放区域
    CAM_STATE_WAITING_REG_QUANTITY = 4, // 等待输入数量
    CAM_STATE_VERIFYING_EXISTING = 5, // ⭐NEW: 验证已存在的资产（显示现有信息）
    CAM_STATE_WAITING_VERIFY_CAPTURE = 6, // ⭐NEW: 等待拍摄正视图进行验证
    CAM_STATE_WAITING_REG_ADD_QTY = 7,    // ⭐NEW: 验证通过后等待输入累加数量
    CAM_STATE_WAITING_INV_TAG_ID = 8, // ⭐ 盘点Tag ID
    CAM_STATE_WAITING_DEL_TAG_ID = 9, // ⭐ 删除Tag ID
    CAM_STATE_WAITING_DEL_CONFIRM = 10, // 等待删除确认
    CAM_STATE_WAITING_OUT_TAG_ID = 11,  // ⭐ 出库Tag ID
    CAM_STATE_WAITING_OUT_QTY = 12,     // 等待出库数量
    CAM_STATE_READY_OUT = 13,           // 出库就绪状态，等待拍摄正视图
    CAM_STATE_READY = 14                // 就绪状态，可以拍摄
} camera_state_t;

// 视图状态枚举（已在 business_executor.h 中定义为 be_view_t）
typedef be_view_t view_state_t;

// 盘点状态枚举
typedef enum {
    INVENTORY_IDLE = 0,
    INVENTORY_WAITING_FRONT,
    INVENTORY_WAITING_SIDE,
    INVENTORY_WAITING_TOP,
    INVENTORY_ANALYZING,
    INVENTORY_COMPLETE  // 添加完成状态
} inventory_state_t;

// 推理任务结构体（拍摄线程 → 推理线程）
typedef struct {
    system_cmd_t view_cmd;          // CMD_CAPTURE_FRONT / SIDE / TOP
    char tag_id[TAG_ID_STR_LEN];    // ⭐ Tag ID替代MAC地址
    int expected_views;             // 期望的总视图数 (注册/盘点=3, 出库=1)
    bool is_registration;           // 注册模式(true: 需保存JPEG, false: 盘点/出库)
    bool must_save_jpeg;            // 是否必须保存JPEG(注册模式)
} inference_job_t;

// ===================================================================
//  全局运行时上下文（精简：14个字段 → 1个结构体）
// ===================================================================
typedef struct {
    // 模块状态
    bool camera_ready;
    bool storage_ready;
    bool storage_initialized;
    bool camera_power_on;

    // 当前任务上下文
    char current_tag_id[TAG_ID_STR_LEN];
    char reg_item_name[128];
    char reg_storage_area;
    uint32_t reg_quantity;
    uint32_t outbound_quantity;
    uint32_t outbound_original_qty;

    // 推理进度
    int views_enqueued;
    int views_processed;
    int total_views;
    bool inference_cancelled;

    // 模式标志
    bool is_inventory_mode;
    bool is_outbound_mode;

    // 状态枚举（UART0 调试 + be_reset_state 共享）
    camera_state_t camera_state;
    view_state_t view_state;
    inventory_state_t inventory_state;

    // L610 4G
    char l610_client_id[64];
} app_context_t;

extern app_context_t g_ctx;

// ===================================================================
//  IPC 句柄（保持独立 — FreeRTOS 惯用法）
// ===================================================================
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern QueueHandle_t xInferenceQueue;
extern SemaphoreHandle_t xCameraMutex;

// ===================================================================
//  特征缓冲区（保持独立数组 — PSRAM 对齐，被多处取地址传递）
// ===================================================================
extern float g_front_feature[];
extern float g_side_feature[];
extern float g_top_feature[];
extern float g_stored_front_feature[];
extern float g_stored_side_feature[];
extern float g_stored_top_feature[];

// ===================================================================
//  UART0 调试专用（verify_handler 模块直接引用）
// ===================================================================
// verify_context_t 声明在 verify_handler.h 中
// extern verify_context_t g_verify_ctx; — 已由 verify_handler.h 声明

// ===================================================================
//  向后兼容宏：旧名称 → g_ctx 字段
//  其他文件无需立即修改，通过宏透明映射
// ===================================================================
#define g_camera_ready          (g_ctx.camera_ready)
#define g_storage_ready         (g_ctx.storage_ready)
#define g_storage_initialized   (g_ctx.storage_initialized)
#define g_camera_power_on       (g_ctx.camera_power_on)
#define g_current_tag_id        (g_ctx.current_tag_id)
#define g_reg_item_name         (g_ctx.reg_item_name)
#define g_reg_storage_area      (g_ctx.reg_storage_area)
#define g_reg_quantity          (g_ctx.reg_quantity)
#define g_outbound_quantity     (g_ctx.outbound_quantity)
#define g_outbound_original_qty (g_ctx.outbound_original_qty)
#define g_views_enqueued        (g_ctx.views_enqueued)
#define g_views_processed       (g_ctx.views_processed)
#define g_total_views           (g_ctx.total_views)
#define g_inference_cancelled   (g_ctx.inference_cancelled)
#define g_is_inventory_mode     (g_ctx.is_inventory_mode)
#define g_is_outbound_mode      (g_ctx.is_outbound_mode)
#define g_camera_state          (g_ctx.camera_state)
#define g_view_state            (g_ctx.view_state)
#define g_inventory_state       (g_ctx.inventory_state)
#define g_l610_client_id        (g_ctx.l610_client_id)

// 外部函数声明
void asset_list_uart(void);
void print_system_info_uart(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
