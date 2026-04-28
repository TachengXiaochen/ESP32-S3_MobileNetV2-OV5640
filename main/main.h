#ifndef MAIN_H
#define MAIN_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "asset_manager.h"

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
    CMD_INFERENCE_TRIGGER   // 推理任务触发：全部视图推理完成后触发最终操作
} system_cmd_t;

// 消息结构体（需要被cmd_handler访问）
typedef struct {
    system_cmd_t cmd;
    void *data;
    char mac[MAC_ADDR_LEN + 1];
} system_msg_t;

// 摄像头状态枚举
typedef enum {
    CAM_STATE_WAITING_MAC = 0,        // 主菜单状态，等待模式选择
    CAM_STATE_WAITING_REG_MAC = 1,    // 等待注册MAC地址
    CAM_STATE_WAITING_REG_NAME = 2,   // 等待输入物品名称
    CAM_STATE_WAITING_REG_AREA = 3,   // 等待输入存放区域
    CAM_STATE_WAITING_REG_QUANTITY = 4, // 等待输入数量
    CAM_STATE_WAITING_INV_MAC = 5,    // 等待盘点MAC地址
    CAM_STATE_WAITING_DEL_MAC = 6,    // 等待删除MAC地址
    CAM_STATE_WAITING_DEL_CONFIRM = 7, // 等待删除确认
    CAM_STATE_WAITING_OUT_MAC = 8,    // 等待出库MAC地址
    CAM_STATE_WAITING_OUT_QTY = 9,    // 等待出库数量
    CAM_STATE_READY_OUT = 10,         // 出库就绪状态，等待拍摄正视图
    CAM_STATE_READY = 11              // 就绪状态，可以拍摄
} camera_state_t;

// 视图状态枚举
typedef enum {
    VIEW_NONE = 0,
    VIEW_FRONT = 1,
    VIEW_SIDE = 2,
    VIEW_TOP = 3
} view_state_t;

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
    char mac[MAC_ADDR_LEN + 1];     // MAC地址
    int expected_views;             // 期望的总视图数 (注册/盘点=3, 出库=1)
    bool is_registration;           // 注册模式(true: 需保存JPEG, false: 盘点/出库)
    bool must_save_jpeg;            // 是否必须保存JPEG(注册模式)
} inference_job_t;

// 外部变量声明（供cmd_handler访问）
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern QueueHandle_t xInferenceQueue;  // 推理任务队列
extern SemaphoreHandle_t xCameraMutex; // 摄像头访问互斥锁
extern char g_current_mac[];
extern camera_state_t g_camera_state;
extern view_state_t g_view_state;
extern inventory_state_t g_inventory_state;
extern float g_front_feature[];
extern float g_side_feature[];
extern float g_top_feature[];
extern bool g_camera_ready;
extern bool g_storage_ready;
extern bool g_is_inventory_mode;    // 区分注册和盘点模式的标志位
extern bool g_is_outbound_mode;     // 区分出库模式的标志位
extern char g_reg_item_name[];      // 注册模式：物品名称
extern char g_reg_storage_area;     // 注册模式：存放区域
extern uint32_t g_reg_quantity;     // 注册模式：数量
extern uint32_t g_outbound_quantity; // 出库模式：出库数量

// 推理任务进度计数器
extern int g_views_enqueued;        // 已入队推理任务数
extern int g_views_processed;       // 已完成推理数
extern int g_total_views;           // 期望总视图数

// 外部函数声明
void asset_list_uart(void);
void print_system_info_uart(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
