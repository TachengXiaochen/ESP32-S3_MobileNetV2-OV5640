# CAM_AI 完全解耦计划 v1.0

> **日期**: 2026-06-05  
> **状态**: 待审阅  
> **前置**: REFACTOR_PLAN.md v3.0 已实施部分  
> **目标**: 移除所有旧文件交叉依赖，新架构完全独立编译

---

## 1. 背景

当前新架构（6 个新文件）已创建，但 `uart_handler_0.c` 仍调用 `cmd_handler_process()`，`main.h` 仍依赖 `protocol_handler.h` 中的 `capture_view_t`。这两个旧文件未从编译清单中移除。

### 需要解决的交叉依赖

| 旧符号 | 来源 | 使用者 | 严重性 |
|--------|------|--------|--------|
| `capture_view_t` + 枚举值 | `protocol_handler.h` | `main.h:79` → `view_state_t`; `main.c` 多处 | **编译阻断** |
| `cmd_handler_process()` | `cmd_handler.c:508` | `uart_handler_0.c:54` | **链接阻断** |
| `show_main_menu()` 等 UI 函数 | `cmd_handler.c` | `main.c:157,246,249,255,257` | **链接阻断** |

---

## 2. 架构变更总览

```
变更前（交叉依赖）:
  main.h ──include──► protocol_handler.h (capture_view_t)
  main.c ──call────► show_*()         (cmd_handler.c)
  uart0  ──call────► cmd_handler_process()

变更后（完全独立）:
  main.h ──include──► business_executor.h (be_view_t, be_error_code_t)
  main.c ──call────► uart_handler_0.h 中的 show_*()
  uart0  ──call────► uart0_dispatch() → be_execute()
```

### 2.1 枚举迁移

| 旧枚举 | 新枚举 | 所在文件 |
|--------|--------|----------|
| `capture_view_t` {VIEW_NONE=0, VIEW_FRONT, VIEW_SIDE, VIEW_TOP} | `be_view_t` {BE_VIEW_NONE=0, BE_VIEW_FRONT, BE_VIEW_SIDE, BE_VIEW_TOP} | `business_executor.h` |
| `ws63_error_t` (29 项) | `be_error_code_t` (保留核心 7 项 + 通用 fallback) | `business_executor.h` |

### 2.2 main.h 中的 view_state_t 重定义

```c
// 旧代码（main.h:79——当前）：
#include "modules/system/protocol_handler.h"  // 需要其中的 capture_view_t
typedef capture_view_t view_state_t;

// 新代码（步骤 2）：
// #include "modules/system/protocol_handler.h"  ← 移除
typedef be_view_t view_state_t;   // be_view_t 在 business_executor.h 中定义
```

---

## 3. 实施步骤

| 步骤 | 文件 | 操作 | 行数 | 参考 |
|------|------|------|------|------|
| **1** | `business_executor.h` | 新增 `be_view_t` 枚举 + `be_error_code_t` 枚举；更新事件中的 `view_index` 和 `error_code` 类型 | +35 | `protocol_handler.h` |
| **2** | `business_executor.c` | 替换所有引用处的枚举值名称 | +0 | 自身 |
| **3** | `main.h` | 移除 `protocol_handler.h` include；`view_state_t` 改为 `typedef be_view_t view_state_t` | +2/-2 | 方案 §7.9 |
| **4** | `uart_handler_0.c` | 完全重写——复制 `cmd_handler.c` 全部函数体，改为调用 `be_execute()` | ~450 | `cmd_handler.c` |
| **5** | `uart_handler_1.c` | 确认 on_event 回调无外部依赖 | 验证 | — |
| **6** | `main.c` | 替换 `VIEW_NONE→BE_VIEW_NONE` 等引用；删除 `uart_task` 函数体 | +10/-50 | — |
| **7** | `CMakeLists.txt` | 从编译清单中移除 `cmd_handler.c` 和 `protocol_handler.c` | -2 | — |
| **8** | 编译验证 | `idf.py build` | — | — |

---

## 4. 各步骤详细说明

### 4.1 business_executor.h — 新增枚举

添加在 `be_channel_t` 定义之后，`be_cmd_t` 之前：

```c
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
    BE_ERR_GENERIC              // 通用 fallback
} be_error_code_t;
```

### 4.2 枚举迁移对照表

| 旧符号 | 新符号 | 出现位置 |
|--------|--------|----------|
| `VIEW_NONE` | `BE_VIEW_NONE` | main.c:76,153; business_executor.c |
| `VIEW_FRONT` | `BE_VIEW_FRONT` | main.c:243 |
| `VIEW_SIDE` | `BE_VIEW_SIDE` | main.c:252 |
| `VIEW_TOP` | `BE_VIEW_TOP` | main.c:258 |
| `capture_view_t` | `be_view_t` | main.h:78-79; business_executor.c 事件结构体和 `be_on_view_captured(int view_index)` |
| `ERR_TASK_BUSY` | `BE_ERR_TASK_BUSY` | business_executor.c:113 |
| `ERR_AI_MODEL_FAIL` | `BE_ERR_AI_MODEL_FAIL` | business_executor.c:165 |
| `ERR_INVALID_TAG_ID` | `BE_ERR_INVALID_TAG_ID` | business_executor.c:147 |

### 4.3 uart_handler_0.c 完全重写——参考 cmd_handler.c 完整函数列表

从 `cmd_handler.c` 复制以下函数体，替换为调用 `be_execute()`：

| 原函数 | 行号 | 新位置 | 改动说明 |
|--------|------|--------|----------|
| `cmd_handler_validate_tag_id` | 43-46 | 删除（直接调 tag_id_validator） | — |
| `show_verification_existing_guide` | 51-67 | 同函数名，UART0 输出 | 无改动 |
| `show_verification_add_qty_guide` | 72-83 | 同上 | 无改动 |
| `show_verification_failed` | 88-99 | 同上 | 无改动 |
| `show_verification_retry_guide` | 104-110 | 同上 | 无改动 |
| `cmd_handler_show_help` | 115-132 | 同上 | 无改动 |
| `show_main_menu` | 137-151 | 同上 | 无改动 |
| `show_registration_mode_guide` | 156-166 | 同上 | 无改动 |
| `show_registration_name_guide` | 171-183 | 同上 | 无改动 |
| `show_registration_area_guide` | 188-198 | 同上 | 无改动 |
| `show_registration_quantity_guide` | 203-211 | 同上 | 无改动 |
| `show_inventory_mode_guide` | 216-226 | 同上 | 无改动 |
| `show_delete_mode_guide` | 231-246 | 同上 | 无改动 |
| `show_registration_step1` | 251-263 | 同上 | 无改动 |
| `show_registration_step2` | 268-274 | 同上 | 无改动 |
| `show_registration_step3` | 280-285 | 同上 | 无改动 |
| `show_inventory_step1` | 290-303 | 同上 | 无改动 |
| `show_inventory_step2` | 309-313 | 同上 | 无改动 |
| `show_inventory_step3` | 319-324 | 同上 | 无改动 |
| `handle_tag_id_initialization` | 430-465 | **关键**——改为调用 `be_execute(BE_CMD_REGISTER)` | **全面改写** |
| `handle_verification_update` | 470-503 | **关键**——改为调用 `be_execute` 验证模式 | **全面改写** |
| `cmd_handler_process` | 508-1228 | **核心**——`uart0_dispatch()` 替代 | **全面改写** |

**核心重构模式**：`cmd_handler_process` 中所有业务操作 → 调用 `be_execute()`。

例如拍正面图 `'f'` 的处理：

```c
// 旧代码（cmd_handler.c:1194-1228）
if (g_camera_state == CAM_STATE_READY) {
    if (view_cmd == 'f') {
        msg.cmd = CMD_CAPTURE_FRONT;
        xQueueSend(xSystemQueue, &msg, portMAX_DELAY);
        if (g_inventory_state == INVENTORY_IDLE) {
            show_registration_step2();
        } else {
            show_inventory_step2();
        }
    }
    // ...
}

// 新代码——完全改为调用 be_execute
if (g_camera_state == CAM_STATE_READY) {
    if (cmd[0] == 'f') {
        be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_FRONT, g_current_tag_id, NULL);
    } else if (cmd[0] == 's') {
        be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_SIDE, g_current_tag_id, NULL);
    } else if (cmd[0] == 't') {
        be_execute(BE_CHANNEL_UART0_TEXT, BE_CMD_CAPTURE_TOP, g_current_tag_id, NULL);
    }
}
```

### 4.4 枚举迁移影响范围

| 文件 | 改动符号 | 说明 |
|------|----------|------|
| `main.h:78-79` | `typedef capture_view_t view_state_t` → `typedef be_view_t view_state_t` | — |
| `main.h:9` | 移除 `#include "modules/system/protocol_handler.h"` | 总行数 134→133 |
| `main.c:67` | `g_inventory_state = INVENTORY_IDLE` | 无改动 |
| `main.c:76` | `view_state_t g_view_state = VIEW_NONE` → `= BE_VIEW_NONE` | — |
| `main.c:153` | `g_view_state = VIEW_NONE` → `= BE_VIEW_NONE` | — |
| `main.c:243` | `g_view_state = VIEW_FRONT` → `= BE_VIEW_FRONT` | — |
| `main.c:252` | `g_view_state = VIEW_SIDE` → `= BE_VIEW_SIDE` | — |
| `main.c:258` | `g_view_state = VIEW_TOP` → `= BE_VIEW_TOP` | — |
| `business_executor.c` | 所有 `be_capture_progress_t.view_index` 已经是 `int` | 无改动 |
| `business_executor.h` | `be_capture_progress_t.view_index = int` — 已经是纯 int，不被枚举限定 | 无改动 |

### 4.5 步骤 7：CMakeLists.txt 移除旧文件

```cmake
# 从 SRCS 中删除以下两行：
# "modules/system/cmd_handler.c" 
# "modules/system/protocol_handler.c"
```

---

## 5. 编译验证（步骤 8）

预期可能的编译错误及修复：

| 错误 | 修复 |
|------|------|
| `unknown type name 'capture_view_t'` | 检查 main.h 已替换为 `be_view_t` |
| `implicit declaration of function 'cmd_handler_process'` | 确认 uart_handler_0.c 不再引用它 |
| `undefined reference to show_*` | 确认 uart_handler_0.c 已包含全部 UI 函数 |
| `unknown type name 'VIEW_NONE'` | 确认 main.c 全部替换为 `BE_VIEW_NONE` |

### 5.1 修改报告

每步完成后在 `docs/archive/DECOUPLING_STEP_X_20260605.md` 中保存报告。

---

## 6. 检查清单

- [ ] `business_executor.h` 新增 `be_view_t` 和 `be_error_code_t`
- [ ] `main.h` 不再 include `protocol_handler.h`
- [ ] `main.c` 全部 `VIEW_*` 替换为 `BE_VIEW_*`
- [ ] `main.c` 删除 `uart_task` 函数体
- [ ] `uart_handler_0.c` 完整复制 `cmd_handler.c` 函数体
- [ ] `uart_handler_0.c` 所有 `f/s/t` 命令改为调用 `be_execute()`
- [ ] `uart_handler_0.c` 所有 `r/c/o/d` 命令改为调用 `be_execute()`
- [ ] `CMakeLists.txt` 移除 `cmd_handler.c` 和 `protocol_handler.c`
- [ ] 编译通过
- [ ] 用户手动删除 4 个旧文件