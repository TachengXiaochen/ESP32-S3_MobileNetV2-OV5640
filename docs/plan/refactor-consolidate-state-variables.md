# 状态机全局变量去重重构方案

> 日期: 2026-06-09 | 状态: 待实施 | 前置: `docs/archive/20260606_INFERENCE_SPEED_OPTIMIZATION.md`

## 背景

项目有两套平行的状态变量系统：

| 系统 | 位置 | 职责 |
|------|------|------|
| Camera App | `main.h` / `main.c` | 摄像头状态、模式标志、Tag ID、视图计数 |
| Business Executor | `business_executor.h` / `.c` | BE 内部状态、任务上下文 |

两套系统有 **7 对**语义重复的变量，各自独立维护，导致多处不同步 bug：

| main.h 变量 | BE 变量 | 问题 |
|-------------|---------|------|
| `g_current_tag_id` | `g_be_tag_id` | 盘点/出库 handler 只设 main 不设 BE → `be_on_all_views_done` 读到空 tag |
| `g_reg_item_name` | `g_be_item_name` | 同上，出库回调返回空名称 |
| `g_reg_storage_area` | `g_be_storage_area` | 同上 |
| `g_reg_quantity` | `g_be_quantity` | BE 的 `be_on_all_views_done` OUTBOUND 读到 0 |
| `g_outbound_quantity` | `g_be_remove_qty` | same concept, different names |
| `g_total_views` | `g_be_total_views` | BE handler 设 BE 的，但 main 的也被同时设了（不一致风险） |
| `g_camera_state` | `g_be_state` | 语义重叠，`CAM_STATE_READY` vs `BE_STATE_WAITING_CAPTURE` 各管各 |

## 方案

**删除 BE 的 6 个重复变量**，统一用 `main.h` 的变量作为唯一数据源。

BE 只保留自身状态机的独有变量：`g_be_state`、`g_be_task`、`g_be_channel`、`g_be_captured_views`、`g_be_is_verify_mode`。

### 映射关系

```
g_be_tag_id       → g_current_tag_id      (char[TAG_ID_STR_LEN])
g_be_item_name    → g_reg_item_name       (char[128])
g_be_storage_area → g_reg_storage_area    (char)
g_be_quantity     → g_reg_quantity        (uint32_t)
g_be_remove_qty   → g_outbound_quantity   (uint32_t)
g_be_total_views  → g_total_views         (int)
```

### 修改文件

#### 文件 1: `main/modules/system/executor/business_executor.h`

**删除** 6 个 extern 声明，**保留** `g_be_state` 和 `g_be_task`：

```diff
- extern char g_be_tag_id[TAG_ID_STR_LEN];
- extern char g_be_item_name[128];
- extern char g_be_storage_area;
- extern uint32_t g_be_quantity;
- extern uint32_t g_be_remove_qty;
```

#### 文件 2: `main/modules/system/executor/business_executor.c`

A. **删除**变量定义：
```diff
- char g_be_tag_id[TAG_ID_STR_LEN] = {0};
- char g_be_item_name[128] = {0};
- char g_be_storage_area = 'A';
- uint32_t g_be_quantity = 0;
- uint32_t g_be_remove_qty = 0;
```

B. **替换**所有引用（约 30 处），集中在以下函数：

| 函数 | 替换 |
|------|------|
| `be_handle_register()` | `g_be_tag_id`→`g_current_tag_id`, `g_be_total_views`→`g_total_views` |
| `be_handle_inventory()` | `g_be_tag_id`→`g_current_tag_id`, `g_be_item_name`→`g_reg_item_name`, `g_be_storage_area`→`g_reg_storage_area`, `g_be_quantity`→`g_reg_quantity`, `g_be_total_views`→`g_total_views` |
| `be_handle_outbound()` | `g_be_tag_id`→`g_current_tag_id`, `g_be_item_name`→`g_reg_item_name`, `g_be_storage_area`→`g_reg_storage_area`, `g_be_quantity`→`g_reg_quantity`, `g_be_remove_qty`→`g_outbound_quantity`, `g_be_total_views`→`g_total_views` |
| `be_handle_capture()` | `g_be_tag_id`→`g_current_tag_id`, `g_be_total_views`→`g_total_views` |
| `be_cancel()` | `g_be_tag_id`→`g_current_tag_id`, `g_be_item_name`→`g_reg_item_name` |
| `be_on_view_captured()` | `g_be_total_views`→`g_total_views` |
| `be_on_all_views_done()` | 所有 6 个变量的所有引用 |
| `be_reset_state()` | 移除 6 个变量的清零（它们现在存在 main.h 中，由 `system_shutdown_camera()` 或 caller 清零） |

C. `be_reset_state()` 改为（同步 main.h 变量）：

```c
static void be_reset_state(void)
{
    g_be_state = BE_STATE_IDLE;
    g_be_task = BE_CMD_UNKNOWN;
    g_be_captured_views = 0;
    g_be_is_verify_mode = false;

    // 同步复位 main.h 全局状态（唯一数据源）
    g_camera_state = CAM_STATE_WAITING_TAG_ID;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    g_inventory_state = INVENTORY_IDLE;
    memset(g_current_tag_id, 0, sizeof(g_current_tag_id));
    memset(g_reg_item_name, 0, sizeof(g_reg_item_name));
    g_reg_storage_area = 'A';
    g_reg_quantity = 0;
    g_outbound_quantity = 0;
    g_total_views = 0;
}
```

#### 文件 3: `main/modules/system/comm/uart_handler_0.c`

盘点 handler 和出库 handler 中最近添加的 `g_be_tag_id`/`g_be_item_name` 等引用 → 对应 main.h 变量：

```diff
  // 盘点 handler
- snprintf(g_be_tag_id, sizeof(g_be_tag_id), "%s", lookup_id);
- snprintf(g_be_item_name, sizeof(g_be_item_name), "%s", record->item_name);
- g_be_storage_area = record->storage_area;
- g_be_quantity = record->quantity;

  // 出库 handler
- g_be_remove_qty = g_outbound_quantity;
```

盘点 handler 原本就通过 `snprintf(g_current_tag_id, ...)` 和 `memcpy(g_stored_*_feature, ...)` 设置了 main.h 变量，BE 的重复设置可以删除。出库 handler 的 `g_be_remove_qty` 就是 `g_outbound_quantity`（已经在第 529 行设置）。

#### 文件 4: `main/main.h` — 无需修改（变量已经存在）

#### 文件 5: `main/main.c` — 无需修改（变量已经定义）

### 不被删除的 BE 独有变量

| 变量 | 保留原因 |
|------|---------|
| `g_be_state` | BE 内部状态机 (IDLE/HARDWARE_INIT/WAITING_CAPTURE/CAPTURING/FINALIZING) |
| `g_be_task` | 当前任务类型 (REGISTER/INVENTORY/OUTBOUND) |
| `g_be_channel` | 命令来源 (UART0_TEXT/UART1_JSON) |
| `g_be_captured_views` | BE 内部视图计数（static） |
| `g_be_is_verify_mode` | 验证模式标志（static） |
| `g_be_cb` | 回调函数指针（static） |

## 影响范围

| 文件 | 改动 |
|------|------|
| `business_executor.h` | 删除 5 行 extern |
| `business_executor.c` | 删除 5 行定义 + 替换 ~30 处引用 + 重写 `be_reset_state()` |
| `uart_handler_0.c` | 删除盘点/出库 handler 中约 5 行重复设置 |

共 **3 文件**，约 50 行改动。不改推理任务、摄像头、AI 模块。

## 风险

- **低**：只是变量名替换，不改变逻辑流程
- `g_reg_item_name` 的命名暗示"注册用"，实际在盘点/出库中也用。可后续重命名为 `g_item_name`
- 需要全文搜索 `g_be_tag_id` 等确保没有遗漏引用

---

## 附：`be_reset_state()` 清空不彻底修复

> 在去重重构中顺便修复

### 当前问题

对比 `system_shutdown_camera()`，`be_reset_state()` 缺少以下清零：

| 变量 | 缺失 | 影响 |
|------|:---:|------|
| `g_view_state` | ❌ | 残留旧视图状态 |
| `g_current_tag_id` | ❌ | 下次操作可能读到旧 tag |
| `g_reg_item_name` | ❌ | 残留旧物品名 |
| `g_reg_quantity` | ❌ | 残留旧数量 |
| `g_outbound_quantity` | ❌ | 残留旧出库量 |
| `g_total_views` | ❌ | 残留旧视图计数 |

### 修复后的 `be_reset_state()`

```c
static void be_reset_state(void)
{
    // BE 内部状态
    g_be_state = BE_STATE_IDLE;
    g_be_task = BE_CMD_UNKNOWN;
    g_be_captured_views = 0;
    g_be_is_verify_mode = false;

    // main.h 全局状态（一次性全部复位）
    g_camera_state = CAM_STATE_WAITING_TAG_ID;
    g_view_state = BE_VIEW_NONE;
    g_inventory_state = INVENTORY_IDLE;
    g_is_inventory_mode = false;
    g_is_outbound_mode = false;
    memset(g_current_tag_id, 0, sizeof(g_current_tag_id));
    g_reg_item_name[0] = '\0';
    g_reg_storage_area = 'A';
    g_reg_quantity = 0;
    g_outbound_quantity = 0;
    g_total_views = 0;
}
```

## 验证

1. `idf.py build`
2. 注册 → 自动回主菜单 ✓
3. 盘点 → `f/s/t` → 显示正确 Tag ID 和物品名 → match/mismatch 区分 ✓
4. 出库 → `f` → 扣减数量正确 ✓
5. 连续多次注册/盘点/出库 → 无残留状态交叉污染 ✓

---

## 附 2：`be_cancel()` 清推理队列不彻底

### 当前问题

```c
// be_cancel() 当前逻辑:
while (xQueueReceive(xInferenceQueue, &discard, 0) == pdTRUE) {}  // 清等待队列
g_views_processed = 0;
// be_reset_state() → g_total_views = 0
```

队列清空后，如果 `inference_task` **正在执行推理**（不在队列中），清队列拦不住。推理完成后：

```c
// inference_task:
g_views_processed++;                           // 0 → 1
if (g_views_processed >= g_total_views) {      // 1 >= 0 → TRUE！
    xQueueSend(xSystemQueue, CMD_INFERENCE_TRIGGER, ...);  // 触发错误的后续操作！
}
```

后果：cancel 后注册可能保存错误资产，盘点可能发出错误匹配结果。

### 修复

#### 文件 1: `main/main.c` — 新增全局标志 + inference_task 检查

```c
// 新增:
bool g_inference_cancelled = false;

// inference_task 改造:
static void inference_task(void *pvParameters)
{
    ...
    while (1) {
        if (xQueueReceive(xInferenceQueue, &job, pdMS_TO_TICKS(2000))) {
            ...
            // 融合输出前检查取消标志
            if (g_inference_cancelled) {
                ESP_LOGI(TAG, "[INF] cancelled, discarding results");
                g_inference_cancelled = false;
                continue;  // 跳过结果处理
            }
            if (fc > 0 && feature_processor_get_fused_feature(fp, ...))
                ...
```

#### 文件 2: `main/main.h` — 声明

```c
extern bool g_inference_cancelled;
```

#### 文件 3: `business_executor.c` — `be_cancel()` 设标志

```c
// 替换 while(xQueueReceive...) 为:
extern bool g_inference_cancelled;
extern QueueHandle_t xInferenceQueue;
inference_job_t discard;
while (xQueueReceive(xInferenceQueue, &discard, 0) == pdTRUE) {}
g_inference_cancelled = true;  // 通知正在执行的推理丢弃结果
g_views_enqueued = 0;
g_views_processed = 0;
```

### 效果

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| cancel 时无推理运行 | ✅ 正常 | ✅ 正常 |
| cancel 时推理在队列中 | ✅ 丢弃 | ✅ 丢弃 |
| cancel 时推理正在执行 | ❌ 残存触发 CMD_INFERENCE_TRIGGER | ✅ 标志位阻止后续操作 |
