# CAM_AI 系统重构方案 v3.0

> **文档版本**: v3.0（完全重建方案）  
> **创建日期**: 2026-06-05  
> **状态**: 待审阅  
> **关联文档**: `docs/PROTOCOL/ESP32_WS63_PROTOCOL.md`  
> **每步修改报告目录**: `docs/archive/REFACTOR_STEP_*.md`

---

## 目录

1. [问题诊断](#1-问题诊断)
2. [重构目标](#2-重构目标)
3. [目标架构](#3-目标架构)
4. [新文件设计](#4-新文件设计)
5. [旧文件废弃清单](#5-旧文件废弃清单)
6. [实施步骤（共 10 步）](#6-实施步骤共-10-步)
7. [各文件详细设计](#7-各文件详细设计)
8. [修改报告规范](#8-修改报告规范)

---

## 1. 问题诊断

### 1.1 现象

ESP32 接收 WS63 的 `register` 命令后（含 AI + Camera 初始化），再收到 `capture` 命令时**卡死**，无法接受后续命令。不启动摄像头时一切正常。

### 1.2 根因

**`ws63_recv_task` 栈溢出。** 8KB 栈必须同时承担 UART 缓冲区（data[1024] + line_buf[1024] = 2KB 常驻）、cJSON 树、AI 模型加载和 MobileNetV2 推理（每帧 `float single_frame[1280]` = 5120 字节 ×3 帧 = 15KB）。

相比之下，已验证正常的 `cmd_handler` 路径通过 `xInferenceQueue` 将推理交由独立的 `inference_task`（8KB 独立栈）异步执行，栈不堆叠。

### 1.3 次要问题

| # | 问题 | 位置 | 严重性 |
|---|------|------|--------|
| 1 | `ws63_handle_capture` 失败时状态不回退（`return` 前已设 `CAPTURING`） | protocol_handler.c:1406-1413 | 中 |
| 2 | WS63 发送 `"asset_list_page"` 而非协议规定的 `"list_assets_page"` | WS63 端 | 低 |
| 3 | 两个 handler 各自维护状态机，代码重复率 > 70% | cmd_handler.c + protocol_handler.c | 严重 |

---

## 2. 重构目标

1. **消除栈溢出**：UART 接收任务绝不执行 AI 推理，统一通过 `xInferenceQueue` 异步分发
2. **消除代码重复**：所有业务逻辑只写一次（`business_executor.c`）
3. **两 UART 独立**：UART0（CLI 调试）和 UART1（WS63 生产）各自独立，互不干扰
4. **低耦合**：handler 只管 I/O 格式，业务逻辑与输出通道解耦

---

## 3. 目标架构

```
┌──────────────────────┐       ┌──────────────────────┐
│  uart_handler_0.c    │       │  uart_handler_1.c    │
│  (UART0 CLI 文本)      │       │  (UART1 WS63 JSON)    │
│  ───────────────────  │       │  ───────────────────  │
│  uart0_task (4KB)     │       │  uart1_task (6KB)    │
│    ├─ uart_read_bytes │       │    ├─ uart_read_bytes │
│    ├─ 文本命令解析      │       │    ├─ cJSON 解析      │
│    ├─ be_execute() ───┼───────┼─── be_execute()     │
│    └─ 回调: 文本输出    │       │    └─ 回调: JSON输出   │
└─────────┬────────────┘       └─────────┬────────────┘
          │                              │
          │      be_response_cb          │
          │  (channel, format, ...)      │
          │                              │
          └──────────┬───────────────────┘
                     ▼
     ┌───────────────────────────────────────┐
     │     business_executor.c (纯逻辑层)      │
     │  ───────────────────────────────────  │
     │  · 统一状态机 (BE_STATE_*)              │
     │  · 参数校验 (tag_id, quantity, ...)     │
     │  · 委托 main.c 队列系统                  │
     │  · 回调通知结果给各自通道                  │
     └──────────┬──────────┬─────────────────┘
                │          │
           xSystemQueue  xInferenceQueue
                │          │
                ▼          ▼
     ┌──────────────┐ ┌──────────────┐
     │camera_ai_task│ │inference_task│  ← main.c 已有基础设施
     │   (8KB)      │ │   (8KB)      │     (无需修改)
     └──────┬───────┘ └──────┬───────┘
            │                │
            └─── 完成后回调 ──┘
                  │
           be_on_task_complete()
                  │
          ┌───────┴───────┐
          ▼               ▼
    uart_handler_0   uart_handler_1
    文本输出 UART0    JSON 输出 UART1
```

### 核心原则

- **UART 任务只做 I/O**：接收 → 解析 → 调用 `be_execute()` → 等待回调 → 格式化输出
- **业务逻辑无通道概念**：`business_executor` 不知道 UART0/UART1，只通过回调函数指针通知结果
- **推理永远在 `inference_task`**：不允许任何 UART 任务显式调用 `camera_module_capture_and_process()`
- **L610 4G 命令保留在 uart_handler_1**：因为 cmd_handler 没有这部分逻辑，没必要抽象到 business_executor

---

## 4. 新文件设计

| # | 文件 | 预计行数 | 职责 |
|---|------|----------|------|
| 1 | `main/modules/system/business_executor.h` | ~150 行 | 业务命令枚举、事件枚举、回调类型、公开 API |
| 2 | `main/modules/system/business_executor.c` | ~500 行 | 状态机、参数校验、`xSystemQueue` 委托、回调通知 |
| 3 | `main/modules/system/uart_handler_0.h` | ~60 行 | `void uart_handler_0_init(void)` + UI 引导函数 |
| 4 | `main/modules/system/uart_handler_0.c` | ~350 行 | UART0 接收任务 + 文本输出回调 + 交互式引导 |
| 5 | `main/modules/system/uart_handler_1.h` | ~50 行 | `void uart_handler_1_init(void)` |
| 6 | `main/modules/system/uart_handler_1.c` | ~400 行 | UART1 接收任务 + JSON 输出回调 + L610 4G 命令 |

---

## 5. 旧文件废弃清单

**以下文件在全部 6 个新文件创建完毕并编译通过后，由你自己删除：**

| 废弃文件 | 替代者 | 说明 |
|----------|--------|------|
| `main/modules/system/cmd_handler.h` | `uart_handler_0.h` | UART0 CLI 处理 |
| `main/modules/system/cmd_handler.c` | `uart_handler_0.c` | UART0 CLI 处理 |
| `main/modules/system/protocol_handler.h` | `uart_handler_1.h` | 大部分类型定义移入 business_executor.h |
| `main/modules/system/protocol_handler.c` | `uart_handler_1.c` | JSON 解析 + L610 4G 保留在 uart_handler_1 |

**重要**：`protocol_handler.h` 中定义的类型（`ws63_cmd_t`、`ws63_error_t`、`capture_view_t`）将被 `business_executor.h` 中的新类型替代。**在废弃之前，不要删除任何旧文件**。

---

## 6. 实施步骤（共 10 步）

| 步骤 | 操作 | 预计行数 | 参考原代码 |
|------|------|----------|-----------|
| **1** | 更新 `main.h` — 添加回调类型定义、BE 相关 extern | +10 行 | main.h 现有 extern + system_cmd_t |
| **2** | 创建 `business_executor.h` | ~150 行 | protocol_handler.h（类型定义部分） + 新设计 |
| **3** | 创建 `business_executor.c` | ~500 行 | cmd_handler.c（业务逻辑） + protocol_handler.c（状态机） |
| **4** | 更新 `main.c` — 注册回调、替换 `#include`、替换 `app_main()` | +20/-20 行 | main.c 现有 app_main + camera_ai_task |
| **5** | 创建 `uart_handler_0.h` | ~60 行 | cmd_handler.h（UI 函数声明） |
| **6** | 创建 `uart_handler_0.c` | ~350 行 | cmd_handler.c（文本解析 + UI 引导） |
| **7** | 创建 `uart_handler_1.h` | ~50 行 | protocol_handler.h（L610 部分） |
| **8** | 创建 `uart_handler_1.c` | ~400 行 | protocol_handler.c（JSON 解析 + L610 4G 命令） |
| **9** | 更新 `CMakeLists.txt` | +3/-2 行 | 添加 3 个新 .c，去掉 2 个旧 .c |
| **10** | 编译验证 + 固件测试 | — | — |

---

## 7. 各文件详细设计

### 7.1 `business_executor.h`（步骤 2）

```c
#ifndef BUSINESS_EXECUTOR_H
#define BUSINESS_EXECUTOR_H

#include <stdbool.h>
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
    BE_EVT_PONG_RESULT,         // 心跳响应
    BE_EVT_ERROR                // 错误
} be_event_t;

// ========== 事件数据（纯数据，不包含格式化）==========

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
void be_on_all_views_done(void);

/**
 * @brief ⭐ 注册 ws63_send_json_raw 函数指针（用于 L610 URC 主动上报）
 * 
 * uart_handler_1 在初始化时调用此函数，让 L610 manager 可以直接往 WS63 发数据。
 */
void be_register_ws63_send_func(void (*func)(const char *));

#ifdef __cplusplus
}
#endif

#endif /* BUSINESS_EXECUTOR_H */
```

### 7.2 `business_executor.c`（步骤 3）— 核心实现

**功能拆解**：

| 功能块 | 参考原代码 | 说明 |
|--------|-----------|------|
| 状态机 | `protocol_handler.c:46-55`（ws63_state_t） | 统一为 `BE_STATE_IDLE / HARDWARE_INIT / WAITING_CAPTURE / CAPTURING / FINALIZING` |
| 任务上下文 | `protocol_handler.c:59-71`（g_ws63_* 全局变量） | 移到模块内 static |
| **be_execute (register)** | `cmd_handler.c:430-465`（handle_tag_id_initialization）+ `cmd_handler.c:734-786`（注册参数收集） | 参数校验后 → ai_module_init() → xSystemQueue → 回调通知 |
| **be_execute (inventory)** | `cmd_handler.c:1046-1107`（等待盘点 Tag ID + 加载特征） | 同上 |
| **be_execute (outbound)** | `cmd_handler.c:789-901`（出库 Tag ID + 数量） | 同上 |
| **be_execute (capture)** | `cmd_handler.c:1194-1228`（f/s/t → xSystemQueue） | 委托给 main.c 的 handle_capture_view |
| **be_execute (delete)** | `protocol_handler.c:1512-1562`（ws63_handle_delete） | 直接调用 asset_delete |
| **be_execute (cancel)** | `cmd_handler.c:529-563`（exit/quit 清理）+ `protocol_handler.c:1568-1600`（ws63_handle_cancel） | 清理 + 回调通知 |
| **be_execute (list/sys_info/ping)** | `protocol_handler.c:1605-1845` | 同步查询，直接回调 |
| **be_on_view_captured** | `main.c:243-259`（视图状态更新逻辑） | 由 main.c handler 调用 |
| **be_on_all_views_done** | `main.c:510-553`（handle_inference_trigger） | 注册/盘点/出库的分支逻辑 |

**关键实现要点**：

```c
// business_executor.c 核心骨架

#include "business_executor.h"
#include "main.h"  // 需要 extern: xSystemQueue, xInferenceQueue, xCameraMutex, g_front/side/top_feature,
                   //              g_reg_item_name, g_reg_storage_area, g_reg_quantity, g_outbound_quantity,
                   //              g_views_enqueued, g_views_processed, g_total_views,
                   //              g_camera_ready, g_is_inventory_mode, g_is_outbound_mode,
                   //              g_inventory_state, g_current_tag_id
#include "modules/ai/ai_module.h"
#include "modules/system/asset_manager.h"
#include "modules/system/tag_id_validator.h"
#include "modules/system/verify_handler.h"

// ===== 状态机 =====
typedef enum {
    BE_STATE_IDLE,
    BE_STATE_HARDWARE_INIT,
    BE_STATE_WAITING_CAPTURE,
    BE_STATE_CAPTURING,
    BE_STATE_FINALIZING
} be_state_t;

static be_response_cb_t g_be_cb = NULL;
static be_channel_t g_be_channel = BE_CHANNEL_UART1_JSON;
static be_state_t g_be_state = BE_STATE_IDLE;
static be_cmd_t g_be_task = BE_CMD_UNKNOWN;

// 任务上下文（复用 cmd_handler 的字段）
static char g_be_tag_id[TAG_ID_STR_LEN] = {0};
static char g_be_item_name[128] = {0};
static char g_be_storage_area = 'A';
static uint32_t g_be_quantity = 0;
static uint32_t g_be_remove_qty = 0;
static int g_be_total_views = 0;
static int g_be_captured_views = 0;
static bool g_be_is_verify_mode = false;

// ===== be_execute 核心流程 =====

esp_err_t be_execute(be_channel_t channel, be_cmd_t cmd, 
                     const char *tag_id, const char *params)
{
    // 0. 设置当前通道
    g_be_channel = channel;

    // 1. 状态检查（cancel 命令是唯一可在 BUSY 态执行的命令）
    if (g_be_state != BE_STATE_IDLE && cmd != BE_CMD_CANCEL) {
        be_error_info_t err = { .error_code = ERR_TASK_BUSY, .error_msg = "Another task is in progress" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_STATE;
    }

    // 2. 根据命令类型分发
    switch (cmd) {
        case BE_CMD_REGISTER:
            return be_handle_register(channel, tag_id, params);
        case BE_CMD_INVENTORY:
            return be_handle_inventory(channel, tag_id, params);
        // ... 其余命令
    }
}

// ===== register 处理（参考 cmd_handler.c:430-465 handle_tag_id_initialization）=====

static esp_err_t be_handle_register(be_channel_t channel, const char *tag_id, const char *params)
{
    // 1. Tag ID 校验（参考 cmd_handler.c:653-670）
    char normalized[TAG_ID_STR_LEN];
    strncpy(normalized, tag_id, TAG_ID_STR_LEN - 1);
    tag_id_validator_normalize(normalized);
    if (!tag_id_validator_validate(normalized)) {
        be_error_info_t err = { .error_code = ERR_INVALID_TAG_ID, .error_msg = tag_id_validator_get_error_string() };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 2. 检查资产是否已存在（协议 §6.1 验证式更新判断）
    asset_record_t existing;
    if (asset_load(normalized, &existing) == ESP_OK && existing.is_valid) {
        // ⭐ 若 params 中有 quantity 但无 item_name → 验证式更新
        // 参考 protocol_handler.c:812-923
        // ... 设置 g_be_is_verify_mode = true, 发送 BE_EVT_HARDWARE_READY
        // 然后进入 VERIFYING 状态
    }
    
    // 3. 保存任务上下文
    strncpy(g_be_tag_id, normalized, sizeof(g_be_tag_id) - 1);
    // 从 params JSON 提取 item_name, storage_area, quantity
    // ... 
    
    g_be_task = BE_CMD_REGISTER;
    g_be_total_views = 3;
    g_be_state = BE_STATE_HARDWARE_INIT;
    
    // 4. 硬件初始化（⭐ AI 模型已在 app_main 中开机加载，此处仅委托摄像头初始化）
    // 注意：be_execute() 不再调用 ai_module_init()，消除 UART 任务阻塞风险
    
    // 同步设置 main.c 全局变量（cmd_handler 也这样做）
    extern bool g_is_inventory_mode;
    extern bool g_is_outbound_mode;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    extern int g_total_views;
    g_total_views = 3;
    extern char g_current_tag_id[];
    snprintf(g_current_tag_id, TAG_ID_STR_LEN, "%s", normalized);
    
    // 通过 xSystemQueue 初始化摄像头（参考 cmd_handler.c:450-453）
    system_msg_t init_msg = {0};
    init_msg.cmd = CMD_INIT_CAMERA;
    snprintf(init_msg.tag_id, sizeof(init_msg.tag_id), "%s", normalized);
    xQueueSend(xSystemQueue, &init_msg, portMAX_DELAY);
    
    // 5. 通知上层：硬件就绪，等待拍摄（参考 protocol_handler.c:1059-1063）
    be_hardware_ready_t ready = {0};
    strncpy(ready.tag_id, normalized, sizeof(ready.tag_id) - 1);
    ready.total_views = 3;
    // ... 填充 item_name, storage_area, quantity
    g_be_cb(channel, BE_EVT_HARDWARE_READY, &ready);
    
    g_be_state = BE_STATE_WAITING_CAPTURE;
    return ESP_OK;
}

// ===== capture 处理（参考 cmd_handler.c:1194-1228）=====

static esp_err_t be_handle_capture(be_channel_t channel, int view_index)
{
    // 状态检查
    if (g_be_state != BE_STATE_WAITING_CAPTURE && g_be_state != BE_STATE_CAPTURING) {
        be_error_info_t err = { .error_code = ERR_INVALID_STATE, .error_msg = "Not in capture state" };
        g_be_cb(channel, BE_EVT_ERROR, &err);
        return ESP_ERR_INVALID_STATE;
    }
    
    g_be_state = BE_STATE_CAPTURING;
    
    // 委托给 main.c 的 xSystemQueue（参考 cmd_handler.c:1198-1203）
    system_cmd_t view_cmd;
    if (view_index == 0) view_cmd = CMD_CAPTURE_FRONT;
    else if (view_index == 1) view_cmd = CMD_CAPTURE_SIDE;
    else view_cmd = CMD_CAPTURE_TOP;
    
    system_msg_t msg = {0};
    msg.cmd = view_cmd;
    snprintf(msg.tag_id, sizeof(msg.tag_id), "%s", g_be_tag_id);
    xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
    
    return ESP_OK;
}

// ===== be_on_view_captured（由 main.c handle_capture_view 调用）=====

void be_on_view_captured(int view_index)
{
    g_be_captured_views++;
    
    // 发送进度回调
    be_capture_progress_t prog = {0};
    strncpy(prog.tag_id, g_be_tag_id, sizeof(prog.tag_id) - 1);
    prog.view_index = view_index;
    prog.total_steps = g_be_total_views;
    g_be_cb(g_be_channel, BE_EVT_CAPTURE_PROGRESS, &prog);
    
    if (g_be_captured_views >= g_be_total_views) {
        // 不在这里做最终操作，等待 inference_task 全部完成后触发 be_on_all_views_done
    }
}

// ===== be_on_all_views_done（由 main.c handle_inference_trigger 调用）=====

void be_on_all_views_done(void)
{
    g_be_state = BE_STATE_FINALIZING;
    
    switch (g_be_task) {
        case BE_CMD_REGISTER: {
            // 参考 main.c:532-553（CMD_INFERENCE_TRIGGER 中注册分支）
            asset_record_t record;
            memset(&record, 0, sizeof(record));
            strncpy(record.tag_id, g_be_tag_id, sizeof(record.tag_id) - 1);
            // ... 填充 item_name, storage_area, quantity, features
            
            bool is_overwrite = false;
            esp_err_t ret = asset_save(&record, &is_overwrite);
            
            be_task_done_t done = {0};
            done.task = BE_CMD_REGISTER;
            strncpy(done.tag_id, g_be_tag_id, sizeof(done.tag_id) - 1);
            // ...
            g_be_cb(g_be_channel, BE_EVT_TASK_DONE, &done);
            break;
        }
        case BE_CMD_INVENTORY: {
            // 参考 main.c:294-375（handle_inventory_analysis）
            // ...
            break;
        }
        case BE_CMD_OUTBOUND: {
            // 参考 main.c:377-507（handle_outbound_analyze + handle_outbound_update_qty）
            // ...
            break;
        }
    }
    
    be_reset_state();
}

// ===== be_reset_state =====

static void be_reset_state(void)
{
    g_be_state = BE_STATE_IDLE;
    g_be_task = BE_CMD_UNKNOWN;
    memset(g_be_tag_id, 0, sizeof(g_be_tag_id));
    memset(g_be_item_name, 0, sizeof(g_be_item_name));
    g_be_storage_area = 'A';
    g_be_quantity = 0;
    g_be_remove_qty = 0;
    g_be_total_views = 0;
    g_be_captured_views = 0;
    g_be_is_verify_mode = false;
}
```

### 7.3 `main.c` 改动（步骤 4）

**最小改动集**（不要大动 main.c 已验证的 handler 函数）：

```c
// ===== #include 替换 =====
// - #include "modules/system/cmd_handler.h"
// - #include "modules/system/protocol_handler.h"
// + #include "modules/system/business_executor.h"
// + #include "modules/system/uart_handler_0.h"
// + #include "modules/system/uart_handler_1.h"

// ===== 输出回调实现（新增，参考 protocol_handler.c ws63_send_json_raw）=====
static void be_output_callback(be_channel_t channel, be_event_t event, const void *data)
{
    if (channel == BE_CHANNEL_UART0_TEXT) {
        uart_handler_0_on_event(event, data);  // 文本输出到 UART0
    } else {
        uart_handler_1_on_event(event, data);  // JSON 输出到 UART1
    }
}

// ===== handle_capture_view 末尾增加回调（参考 main.c:243-259）=====
// 在视图状态更新后：
be_on_view_captured(view_index);

// ===== handle_inference_trigger 开头增加回调（参考 main.c:510-553）=====
// 在 switch 之前：
be_on_all_views_done();

// ===== app_main() 替换 =====
// 旧代码:
//   protocol_handler_init();
//
// 新代码:
//   ai_module_init();              // ⭐ 开机一次性加载 AI 模型（与SD卡同策略）
//   be_init(be_output_callback);
//   uart_handler_0_init();
//   uart_handler_1_init();
//
// 注意: storage_module_init() 已在 app_main 中提前调用（保持不变）
//       uart_task (UART0 接收) 不再由 main.c 创建，改为 uart_handler_0_init() 内部创建

// - xTaskCreate(uart_task, ...)   // 旧 UART0 接收任务 → 删除此行
// + 由 uart_handler_0_init() 内部创建

// 其余 main.c 代码不变（camera_ai_task, inference_task, storage_task 全部保留）
```

### 7.4 `uart_handler_0.h`（步骤 5）

```c
#ifndef UART_HANDLER_0_H
#define UART_HANDLER_0_H

#include "business_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 UART0 CLI 处理器
 * 
 * 创建 uart0_recv_task，注册输出回调。
 */
void uart_handler_0_init(void);

/**
 * @brief 处理 business_executor 发来的事件，格式化并输出到 UART0
 */
void uart_handler_0_on_event(be_event_t event, const void *data);

// ===== UI 引导函数（从 cmd_handler.h 迁移）=====
void show_main_menu(void);
void show_registration_step1(const char *tag_id);
void show_registration_step2(void);
void show_registration_step3(void);
void show_inventory_step1(const char *tag_id);
void show_inventory_step2(void);
void show_inventory_step3(void);
void show_verification_existing_guide(const char *tag_id, const char *item_name,
                                       char storage_area, uint32_t current_qty);
void show_verification_add_qty_guide(const char *tag_id, const char *item_name,
                                      uint32_t current_qty);
void show_verification_failed(float confidence, float threshold);
void show_verification_retry_guide(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_0_H */
```

### 7.5 `uart_handler_0.c`（步骤 6）

**完全参考 `cmd_handler.c` 的以下部分**：

| 功能 | 原代码位置 | 新代码说明 |
|------|-----------|-----------|
| UART 接收任务 | cmd_handler 没有（直接在 uart_task 中）→ 新建 | 复制 main.c:556-588 的 uart_task 模式 |
| 文本命令解析 | `cmd_handler.c:508-527` | 去除前后空格 |
| 主菜单命令分发 | `cmd_handler.c:603-648` | `r`/`c`/`o`/`d` → `be_execute()` |
| Tag ID 输入处理 | `cmd_handler.c:650-703` | Tag ID 校验 → 查重 → 验证更新分支 |
| 注册参数收集 | `cmd_handler.c:706-786` | name → area → quantity 逐步引导 |
| 出库参数收集 | `cmd_handler.c:789-901` | Tag ID 验证 → 数量输入 |
| 删除流程 | `cmd_handler.c:1110-1191` | Tag ID → 确认 → 删除 |
| 验证式更新 | `cmd_handler.c:903-967` (f/q 拍照验证) + `cmd_handler.c:969-1042` (数量累加) | 完全保留 |
| 拍摄命令 f/s/t | `cmd_handler.c:1194-1228` | `be_execute(BE_CMD_CAPTURE_FRONT/SIDE/TOP)` |
| **on_event 回调** | 新建 | 根据 event 类型 format 为文本写到 UART0；参考 protocol_handler 的 `protocol_generate_*` 函数但输出格式不同 |
| UI 引导函数 | `cmd_handler.c:51-99` (show_*) + `cmd_handler.c:137-324` (show_*_step*) | 完全保留，文本输出函数 |

**uart_handler_0.c 核心骨架**：

```c
#include "uart_handler_0.h"
#include "main.h"  // g_camera_state, g_current_tag_id, xSystemQueue, xStorageQueue
#include "modules/system/tag_id_validator.h"
#include "modules/system/asset_manager.h"

static const char *TAG = "uart0_handler";

// ===== UART0 接收任务（完全复制 main.c:556-588 uart_task）=====
static void uart0_recv_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    uint8_t *data = (uint8_t *)malloc(1024 * 2);
    char line_buf[128] = {0};
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM_0, data, 1024 * 2, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t ch = data[i];
                if (ch == '\r' || ch == '\n') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received: %s", line_buf);
                        uart0_dispatch(line_buf);  // ← 替换原来的 cmd_handler_process()
                        line_pos = 0;
                        memset(line_buf, 0, sizeof(line_buf));
                    }
                } else {
                    if (line_pos < sizeof(line_buf) - 1)
                        line_buf[line_pos++] = ch;
                }
            }
        }
        esp_task_wdt_reset();
    }
    free(data);
    vTaskDelete(NULL);
}

// ===== 命令分发（参考 cmd_handler.c:508-527）=====
static void uart0_dispatch(const char *cmd_line)
{
    // ... 去除空格 → 根据 g_camera_state 分发
    
    // 主菜单状态（参考 cmd_handler.c:603-648）
    if (g_camera_state == CAM_STATE_WAITING_TAG_ID) {
        if (strcasecmp(cmd_trimmed, "r") == 0) {
            g_camera_state = CAM_STATE_WAITING_REG_TAG_ID;
            // ... 显示注册引导
            be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_REGISTER, NULL, NULL);
            // 实际上 UART0 的流程需要先收集参数再 be_execute
        }
        // ... c/o/d/l/i 命令
    }
    
    // 等待名称/区域/数量输入状态
    // 等待 capture f/s/t 状态（参考 cmd_handler.c:1194-1228）
    if (g_camera_state == CAM_STATE_READY) {
        if (cmd_trimmed[0] == 'f') be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_FRONT, g_current_tag_id, NULL);
        else if (cmd_trimmed[0] == 's') be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_SIDE, g_current_tag_id, NULL);
        else if (cmd_trimmed[0] == 't') be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_TOP, g_current_tag_id, NULL);
    }
}

// ===== on_event 回调（文本格式化输出）=====
void uart_handler_0_on_event(be_event_t event, const void *data)
{
    char buf[512];
    
    switch (event) {
        case BE_EVT_HARDWARE_READY: {
            const be_hardware_ready_t *r = (const be_hardware_ready_t *)data;
            if (r->total_views == 3) {
                show_registration_step1(r->tag_id);  // 复用现有 UI 函数
            } else {
                show_inventory_step1(r->tag_id);
            }
            break;
        }
        case BE_EVT_CAPTURE_PROGRESS: {
            const be_capture_progress_t *p = (const be_capture_progress_t *)data;
            snprintf(buf, sizeof(buf), "[STEP %d/%d] %s view captured, blur=%.1f\r\n",
                     p->view_index + 1, p->total_steps,
                     p->view_index == 0 ? "Front" : (p->view_index == 1 ? "Side" : "Top"),
                     p->blur_score);
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            break;
        }
        case BE_EVT_TASK_DONE: {
            const be_task_done_t *d = (const be_task_done_t *)data;
            snprintf(buf, sizeof(buf),
                     "\r\n✅ TASK DONE: %s\r\n  Tag ID: %s\r\n  Result: success\r\n\r\n",
                     d->task == BE_CMD_REGISTER ? "Register" : "Inventory",
                     d->tag_id);
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            show_main_menu();
            break;
        }
        case BE_EVT_ERROR: {
            const be_error_info_t *e = (const be_error_info_t *)data;
            snprintf(buf, sizeof(buf), "[ERROR] %s\r\n", e->error_msg);
            uart_write_bytes(UART_NUM_0, buf, strlen(buf));
            break;
        }
        // ... 其余事件
    }
}

// ===== 初始化 =====
void uart_handler_0_init(void)
{
    // UART0 已在 app_main 中由 init_uart() 初始化，这里只创建接收任务
    xTaskCreate(uart0_recv_task, "uart0_recv_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART0 CLI handler initialized");
}
```

### 7.6 `uart_handler_1.h`（步骤 7）

```c
#ifndef UART_HANDLER_1_H
#define UART_HANDLER_1_H

#include "business_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 UART1 WS63 协议处理器
 * 
 * - 初始化 UART1 硬件（GPIO17 TX, GPIO18 RX, 115200）
 * - 创建 ws63_recv_task（6KB 栈）
 * - 注册 L610 主动上报回调
 */
void uart_handler_1_init(void);

/**
 * @brief 处理 business_executor 发来的事件，格式化为 WS63 JSON 协议并输出到 UART1
 */
void uart_handler_1_on_event(be_event_t event, const void *data);

/**
 * @brief 发送 JSON 字符串到 WS63 UART1（供 L610 URC 回调使用）
 */
void uart_handler_1_send_json(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_1_H */
```

### 7.7 `uart_handler_1.c`（步骤 8）

**完全参考 `protocol_handler.c` 的以下部分**：

| 功能 | 原代码位置 | 新代码说明 |
|------|-----------|-----------|
| UART1 硬件初始化 | `protocol_handler.c:651-685`（ws63_uart_init） | 完全保留 |
| UART1 接收任务 | `protocol_handler.c:2191-2245`（ws63_recv_task） | **栈从 8192 改为 6144** |
| JSON 命令解析 + 命令映射表 | `protocol_handler.c:226-320`（protocol_handle_command） | 保留 ws63_cmd → be_cmd 映射表 |
| **on_event 回调** | 新建 | 参考 `protocol_generate_*` 系列函数生成 JSON |
| L610 4G 命令处理 | `protocol_handler.c:2247-2610`（ws63_handle_l610_*） | **完全保留**，不经过 business_executor |
| 发送 JSON 到 UART1 | `protocol_handler.c:690-713`（ws63_send_json_raw） | 完全保留 |

**删除的部分（不再需要的）**：

| 原函数 | 替代 |
|--------|------|
| `ws63_handle_register()` (800-1072) | `be_execute(BE_CMD_REGISTER)` |
| `ws63_handle_inventory()` (1077-1195) | `be_execute(BE_CMD_INVENTORY)` |
| `ws63_handle_outbound()` (1200-1312) | `be_execute(BE_CMD_OUTBOUND)` |
| `ws63_handle_capture()` (1317-1436) | `be_execute(BE_CMD_CAPTURE_*)` |
| `ws63_capture_and_process()` (1850-1950) | **删除**—推理走 xInferenceQueue |
| `ws63_finalize_task()` (1955-2164) | 移到 `business_executor.c` 的 `be_on_all_views_done()` |
| `ws63_reset_state()` (2169-2186) | 移到 `business_executor.c` 的 `be_reset_state()` |
| `ws63_handle_delete()` (1512-1562) | `be_execute(BE_CMD_DELETE)` |
| `ws63_handle_cancel()` (1568-1600) | `be_execute(BE_CMD_CANCEL)` |
| `ws63_handle_list_assets/page/get_asset/sys_info/ping` | 对应 `be_execute()` 各命令 |

**uart_handler_1.c 核心骨架**：

```c
#include "uart_handler_1.h"
#include "main.h"  // WS63_UART_TX_PIN, WS63_UART_RX_PIN & WS63_UART_BUF_SIZE macros
#include "cJSON.h"
#include "modules/4g/l610_manager.h"
#include "modules/4g/l610_mqtt.h"
#include "modules/4g/l610_driver.h"

static const char *TAG = "uart1_handler";

// ===== UART 配置 =====
#define WS63_UART_NUM    UART_NUM_1
#define WS63_UART_BAUD   115200

// ===== L610 连接信息（用于上报）=====
static char g_l610_last_host[128] = "unknown";
static uint16_t g_l610_last_port = 1883;

// ===== UART1 初始化（完全复制 ws63_uart_init）=====
static esp_err_t uart1_uart_init(void)
{
    // 同 protocol_handler.c:651-685
}

// ===== 发送 JSON（完全复制 ws63_send_json_raw）=====
void uart_handler_1_send_json(const char *json_str)
{
    // 同 protocol_handler.c:690-713
}

// ===== UART1 接收任务（复制 ws63_recv_task，栈改为 6144）=====
static void uart1_recv_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    uint8_t *data = (uint8_t *)malloc(WS63_UART_BUF_SIZE);   // 1024
    char *line_buf = (char *)malloc(WS63_UART_BUF_SIZE);     // 1024
    int line_pos = 0;

    while (1) {
        int len = uart_read_bytes(WS63_UART_NUM, data, WS63_UART_BUF_SIZE, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                if (data[i] == '\n' || data[i] == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        ESP_LOGI(TAG, "Received from WS63: %s", line_buf);
                        uart1_dispatch(line_buf);  // ← 替换 protocol_handle_command
                        line_pos = 0;
                    }
                } else {
                    if (line_pos < WS63_UART_BUF_SIZE - 1)
                        line_buf[line_pos++] = data[i];
                }
            }
        }
        esp_task_wdt_reset();
    }
}

// ===== JSON 命令分发（参考 protocol_handle_command）=====
static void uart1_dispatch(const char *json_str)
{
    cJSON *json_obj = cJSON_Parse(json_str);
    if (!json_obj) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"INVALID_JSON\",\"msg\":\"JSON parse error\"}");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(json_obj, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"MISSING_FIELD\",\"msg\":\"Missing cmd field\"}");
        cJSON_Delete(json_obj);
        return;
    }

    const char *cmd_str = cmd_item->valuestring;

    // ===== WS63 命令 → BE 命令映射表 =====
    static const struct { const char *ws63_cmd; be_cmd_t be_cmd; } cmd_map[] = {
        {"register",         BE_CMD_REGISTER},
        {"inventory",        BE_CMD_INVENTORY},
        {"outbound",         BE_CMD_OUTBOUND},
        {"capture",          BE_CMD_CAPTURE_FRONT},      // view 从 params 提取
        {"delete",           BE_CMD_DELETE},
        {"cancel",           BE_CMD_CANCEL},
        {"list_assets",      BE_CMD_LIST_ASSETS},
        {"list_assets_page", BE_CMD_LIST_ASSETS_PAGE},
        {"asset_list_page",  BE_CMD_LIST_ASSETS_PAGE},   // ← 兼容 WS63 端拼写错误
        {"get_asset",        BE_CMD_GET_ASSET},
        {"sys_info",         BE_CMD_SYS_INFO},
        {"ping",             BE_CMD_PING},
    };

    for (int i = 0; i < sizeof(cmd_map)/sizeof(cmd_map[0]); i++) {
        if (strcmp(cmd_str, cmd_map[i].ws63_cmd) == 0) {
            // capture 命令需要解析 view 字段映射到具体视图命令
            be_cmd_t be_cmd = cmd_map[i].be_cmd;
            if (strcmp(cmd_str, "capture") == 0) {
                cJSON *view_item = cJSON_GetObjectItem(json_obj, "view");
                if (view_item && cJSON_IsString(view_item)) {
                    if (strcmp(view_item->valuestring, "front") == 0) be_cmd = BE_CMD_CAPTURE_FRONT;
                    else if (strcmp(view_item->valuestring, "side") == 0) be_cmd = BE_CMD_CAPTURE_SIDE;
                    else if (strcmp(view_item->valuestring, "top") == 0) be_cmd = BE_CMD_CAPTURE_TOP;
                }
            }

            // 提取 tag_id
            cJSON *tag_item = cJSON_GetObjectItem(json_obj, "tag_id");
            const char *tag_id = tag_item ? tag_item->valuestring : NULL;

            // 序列化整个 JSON 作为 params 传给 business_executor
            char *params = cJSON_PrintUnformatted(json_obj);
            be_execute(BE_CHANNEL_UART1_JSON, be_cmd, tag_id, params);
            free(params);
            cJSON_Delete(json_obj);
            return;
        }
    }

    // ===== L610 4G 命令（不经过 business_executor，直接处理）=====
    if (strcmp(cmd_str, "mqtt_connect") == 0) {
        uart1_handle_l610_connect(json_obj);
    } else if (strcmp(cmd_str, "mqtt_disconnect") == 0) {
        uart1_handle_l610_disconnect(json_obj);
    } else if (strcmp(cmd_str, "mqtt_publish") == 0) {
        uart1_handle_l610_publish(json_obj);
    } else if (strcmp(cmd_str, "l610_status") == 0) {
        uart1_handle_l610_status();
    } else if (strcmp(cmd_str, "l610_at") == 0) {
        uart1_handle_l610_at(json_obj);
    } else if (strcmp(cmd_str, "l610_mqtt_check") == 0) {
        uart1_handle_l610_mqtt_check();
    } else {
        ESP_LOGE(TAG, "Unknown command: %s", cmd_str);
        uart_handler_1_send_json("{\"type\":\"error\",\"code\":\"UNKNOWN_CMD\",\"msg\":\"Unknown command\"}");
    }

    cJSON_Delete(json_obj);
}

// ===== on_event 回调（JSON 格式化输出）=====
void uart_handler_1_on_event(be_event_t event, const void *data)
{
    switch (event) {
        case BE_EVT_HARDWARE_READY: {
            const be_hardware_ready_t *r = (const be_hardware_ready_t *)data;
            // 格式化为 capture_progress JSON（参考 protocol_generate_capture_progress）
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "capture_progress");
            cJSON_AddStringToObject(msg, "tag_id", r->tag_id);
            cJSON_AddStringToObject(msg, "view", "none");
            cJSON_AddStringToObject(msg, "step", "0/3");
            cJSON_AddStringToObject(msg, "status", "ready");
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_CAPTURE_PROGRESS: {
            const be_capture_progress_t *p = (const be_capture_progress_t *)data;
            // 参考 protocol_generate_capture_progress
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "capture_progress");
            cJSON_AddStringToObject(msg, "tag_id", p->tag_id);
            cJSON_AddStringToObject(msg, "view",
                p->view_index == 0 ? "front" : (p->view_index == 1 ? "side" : "top"));
            char step[8];
            snprintf(step, sizeof(step), "%d/%d", p->view_index + 1, p->total_steps);
            cJSON_AddStringToObject(msg, "step", step);
            cJSON_AddStringToObject(msg, "status", "ok");
            cJSON_AddNumberToObject(msg, "blur_score", p->blur_score);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_TASK_DONE: {
            const be_task_done_t *d = (const be_task_done_t *)data;
            // 参考 protocol_generate_task_done + ws63_finalize_task 中的 JSON 构建
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "task_done");
            cJSON_AddStringToObject(msg, "task",
                d->task == BE_CMD_REGISTER ? "register" :
                d->task == BE_CMD_INVENTORY ? "inventory" : "outbound");
            cJSON_AddStringToObject(msg, "tag_id", d->tag_id);
            cJSON_AddStringToObject(msg, "result", "success");
            cJSON_AddStringToObject(msg, "item_name", d->item_name);
            cJSON_AddNumberToObject(msg, "quantity", d->quantity);
            if (d->task == BE_CMD_INVENTORY || d->task == BE_CMD_OUTBOUND) {
                cJSON_AddBoolToObject(msg, "is_match", d->is_match);
                cJSON_AddNumberToObject(msg, "confidence", d->confidence);
                cJSON_AddNumberToObject(msg, "threshold", d->threshold);
            }
            if (d->task == BE_CMD_OUTBOUND) {
                cJSON_AddNumberToObject(msg, "original_qty", d->previous_qty);
                cJSON_AddNumberToObject(msg, "remove_qty", d->remove_qty);
                cJSON_AddNumberToObject(msg, "remaining_qty", d->quantity);
            }
            if (d->is_verify_mode) {
                cJSON_AddNumberToObject(msg, "previous_qty", d->previous_qty);
                cJSON_AddNumberToObject(msg, "added_qty", d->quantity - d->previous_qty);
                cJSON_AddNumberToObject(msg, "new_qty", d->quantity);
            }
            cJSON_AddBoolToObject(msg, "is_overwrite", d->is_overwrite);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        case BE_EVT_ERROR: {
            const be_error_info_t *e = (const be_error_info_t *)data;
            // 参考 protocol_generate_error_response
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "error");
            cJSON_AddStringToObject(msg, "code", "ERR_UNKNOWN");  // TODO: 错误码映射
            cJSON_AddStringToObject(msg, "msg", e->error_msg);
            char *js = cJSON_PrintUnformatted(msg);
            uart_handler_1_send_json(js);
            free(js);
            cJSON_Delete(msg);
            break;
        }
        // ... BE_EVT_ASSET_LIST_RESULT, BE_EVT_ASSET_DETAIL, BE_EVT_SYS_INFO_RESULT, BE_EVT_PONG_RESULT
    }
}

// ===== 初始化 =====
void uart_handler_1_init(void)
{
    uart1_uart_init();

    // 注册 L610 主动上报回调
    extern void l610_manager_register_send_func(void (*)(const char *));
    l610_manager_register_send_func(uart_handler_1_send_json);

    // L610 ClientID 生成委托给 business_executor（或在此处生成）
    // 参考 protocol_handler.c:1022

    // 创建接收任务（栈=6144，仅 I/O + cJSON，不再执行 AI 推理）
    xTaskCreate(uart1_recv_task, "uart1_recv_task", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART1 WS63 handler initialized");
}

// ===== L610 4G 命令处理函数（从 protocol_handler.c:2247-2610 完全复制）=====
// 以下函数保持原样，不需要通过 business_executor：
// - uart1_handle_l610_connect()    (原 ws63_handle_l610_connect)
// - uart1_handle_l610_disconnect() (原 ws63_handle_l610_disconnect)
// - uart1_handle_l610_publish()    (原 ws63_handle_l610_publish)
// - uart1_handle_l610_status()     (原 ws63_handle_l610_status)
// - uart1_handle_l610_at()         (原 ws63_handle_l610_at)
// - uart1_handle_l610_mqtt_check() (原 ws63_handle_l610_mqtt_check)
```

### 7.8 `CMakeLists.txt` 改动（步骤 9）

```cmake
idf_component_register(SRCS "modules/system/business_executor.c"    # ← 新增
                             "modules/system/uart_handler_0.c"      # ← 新增
                             "modules/system/uart_handler_1.c"      # ← 新增
                             "main.c" 
                             "modules/system/asset_manager.c" 
                             "modules/system/tag_id_validator.c"
                             "modules/system/verify_handler.c"
                             "modules/camera/camera_module.c" 
                             "modules/system/storage_module.c" 
                             "modules/ai/ai_module.c" 
                             "modules/ai/mobilenet_wrapper.cpp" 
                             # "modules/system/cmd_handler.c"       # ← 删除
                             # "modules/system/protocol_handler.c"  # ← 删除
                             "modules/system/led_indicator.c" 
                             "modules/ai/feature_processor.c" 
                             "modules/ai/similarity_matcher.c" 
                             "modules/ai/blur_detection.c" 
                             "modules/4g/l610_driver.c" 
                             "modules/4g/l610_mqtt.c" 
                             "modules/4g/l610_manager.c"
                     INCLUDE_DIRS "." "modules/ai" "modules/camera" "modules/system" "modules/4g"
                     REQUIRES esp32-camera esp-dl imagenet_cls fatfs sdmmc nvs_flash driver json)
```

### 7.9 `main.h` 改动（步骤 1）

```c
// ===== 新增 include =====
#include "modules/system/business_executor.h"

// ===== 现有 extern 声明全部保留，新增 =====
extern QueueHandle_t xSystemQueue;
extern QueueHandle_t xStorageQueue;
extern QueueHandle_t xInferenceQueue;
extern SemaphoreHandle_t xCameraMutex;
extern char g_current_tag_id[];      // ⭐ business_executor 需要
extern camera_state_t g_camera_state;
extern view_state_t g_view_state;
extern inventory_state_t g_inventory_state;
extern float g_front_feature[];      // ⭐ business_executor 保存/匹配时需要
extern float g_side_feature[];
extern float g_top_feature[];
extern bool g_camera_ready;
extern bool g_storage_ready;
extern bool g_is_inventory_mode;
extern bool g_is_outbound_mode;
extern char g_reg_item_name[];       // ⭐ business_executor 保存时需要
extern char g_reg_storage_area;
extern uint32_t g_reg_quantity;
extern uint32_t g_outbound_quantity;
extern int g_views_enqueued;
extern int g_views_processed;
extern int g_total_views;
```

---

## 8. 修改报告规范

每完成一个子步骤，在 `docs/archive/` 下生成修改报告，命名格式：

```
docs/archive/REFACTOR_STEP_{步骤序号}_{日期}.md
```

例如：
- `docs/archive/REFACTOR_STEP_01_20260605.md` — 步骤 1：更新 main.h
- `docs/archive/REFACTOR_STEP_02_20260605.md` — 步骤 2：创建 business_executor.h

**报告模板**：

```markdown
# 重构修改报告 — 第 X 步：{步骤名称}

| 属性 | 值 |
|------|-----|
| 日期 | YYYY-MM-DD |
| 步骤 | X / 10 |
| 操作类型 | 新建文件 / 修改文件 / 删除文件 |
| 文件(改动前) | {文件路径} |
| 文件(改动后) | {文件路径} |
| 改动行数 | +X 行 / -Y 行 |
| 参考原代码 | {具体文件和行号} |

## 修改前状态

{简述问题或当前状态}

## 修改内容

{详细描述所有改动}

## 修改后状态

{如何验证修改正确}

## 风险提示

{如有}
```

---

## 9. 编译验证

步骤 10 为编译验证。预期：

```bash
cd d:/Users/TcXc/Desktop/Program_ESP32-S3CAM/CAM_AI
idf.py build
```

**预期编译错误及修复**（按可能出现的顺序）：

| 预期错误 | 修复方式 |
|----------|----------|
| `undefined reference to be_execute` | 确认 CMakeLists.txt 包含 business_executor.c |
| `unknown type name 'be_channel_t'` | 确认 business_executor.h 被 include |
| `multiple definition of g_camera_state` | 检查 main.h extern 声明不冲突 |
| `implicit declaration of function 'tag_id_validator_*'` | 确认 include 路径 |
| `implicit declaration of function 'l610_*'` | 确认 uart_handler_1.c include 了 l610 相关头文件 |

---

## 10. 审阅检查清单

请确认以下内容：

- [x] 架构方案是否合理（两个 handler + 一个 executor）
- [x] `business_executor.h` API 设计是否满足需求
- [x] `uart_handler_1.c` 中 L610 4G 命令不从 business_executor 过是否合理
- [x] `main.c` 改动是否最小化（handler 函数基本不改）
- [x] 分 10 步实施的顺序是否合理
- [x] 每步只新建一个文件是否粒度合适
- [x] 修改报告格式是否满足要求
- [x] **ai_module_init() 开机加载策略**（已确认并更新）
  - `ai_module_init()` 已在 `app_main()` 中于 `storage_module_init()` 之后调用
  - AI 模型常驻内存，`be_execute()` 不再调用 `ai_module_init()`
  - UART 任务零阻塞，`be_execute()` 收到命令后立即入队到 `xSystemQueue` 并返回
- [x] **business_executor.c 的 extern 依赖是否太多**
  - 当前设计通过 extern 访问 main.c 的全局变量（g_* 系列）
  - 这是 **FreeRTOS 资源受限嵌入式系统的常见模式**，避免过度抽象增加内存开销
  - 替代方案（消息传递）会引入额外的拷贝和队列开销
