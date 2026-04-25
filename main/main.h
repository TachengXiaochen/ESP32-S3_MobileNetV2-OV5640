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
    CMD_INVENTORY_WITH_MAC
} system_cmd_t;

// 消息结构体（需要被cmd_handler访问）
typedef struct {
    system_cmd_t cmd;
    void *data;
    char mac[MAC_ADDR_LEN + 1];
} system_msg_t;

// 摄像头状态枚举
typedef enum {
    CAM_STATE_WAITING_MAC = 0,      // 主菜单状态，等待模式选择
    CAM_STATE_WAITING_REG_MAC = 1,  // 等待注册MAC地址
    CAM_STATE_WAITING_INV_MAC = 2,  // 等待盘点MAC地址
    CAM_STATE_READY = 3             // 就绪状态，可以拍摄
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

// 外部变量声明（供cmd_handler访问）
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern char g_current_mac[];
extern camera_state_t g_camera_state;
extern view_state_t g_view_state;
extern inventory_state_t g_inventory_state;
extern float g_front_feature[];
extern float g_side_feature[];
extern float g_top_feature[];
extern bool g_camera_ready;
extern bool g_storage_ready;
extern bool g_is_inventory_mode;  // ✅ 新增：区分注册和盘点模式的标志位

// 外部函数声明
void asset_list_uart(void);
void print_system_info_uart(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
