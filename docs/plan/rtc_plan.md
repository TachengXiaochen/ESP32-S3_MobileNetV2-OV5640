# RTC 唤醒功能实现方案

> **文档状态**: 计划阶段 (未实现)  
> **最后更新**: 2026-06-04  
> **适用项目**: CAM_AI (ESP32-S3 视觉感知物资管理子节点)  

---

## 1. 硬件连接

根据 `PROTOCOL.md` §2.1，已有接线：

| 信号 | ESP32-S3 引脚 | WS63 引脚 | 方向 | 说明 |
|------|-------------|----------|------|------|
| RTC 唤醒 | **GPIO2** | GPIO (推挽输出) | WS63 → ESP32 | 拉高唤醒 ESP32，拉低允许睡眠 |

### WS63 端行为约定

| GPIO2 状态 | ESP32 行为 |
|-----------|-----------|
| **高电平** | 唤醒/保持唤醒（ESP32 正常工作） |
| **低电平** | 允许睡眠（ESP32 可在空闲时进入 deep sleep） |

> WS63 在需要 ESP32 工作时拉高 GPIO2，在 ESP32 空闲后拉低 GPIO2 表示可以睡眠。

---

## 2. 当前状态

### 代码现状

- `app_main()` 末尾只是个死循环 `vTaskDelay(pdMS_TO_TICKS(1000))`
- ESP32-S3 全程全速运行，不进入任何睡眠状态
- 所有 4 个任务（`uart_task`/`camera_ai_task`/`inference_task`/`storage_task`）和 1 个额外任务（`ws63_recv_task`）持续运行
- Task Watchdog (TWDT) 已开启

### sdkconfig 已启用的睡眠相关配置

```
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_MSPI_NEED_ALL_IO_PU=y
CONFIG_ESP_SLEEP_RTC_BUS_ISO_WORKAROUND=y
CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y
CONFIG_ESP_SLEEP_WAIT_FLASH_READY_EXTRA_DELAY=2000
CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=y

CONFIG_PM_SUPPORT_EXT0_WAKEUP=y
CONFIG_PM_SUPPORT_EXT1_WAKEUP=y
CONFIG_PM_SUPPORT_EXT_WAKEUP=y
```

已具备进入 deep sleep 和外部唤醒的基础配置。

---

## 3. 实现方案

### 3.1 核心文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `main/main.c` | **新增** | sleep 相关函数 + main loop 睡眠逻辑 |
| `main/main.h` | **新增** | 函数声明 |
| `sdkconfig` | **修改** | 启用 `CONFIG_ESP_TASK_WDT=n` (睡眠前禁 TWDT) |
| (可选) `protocol_handler.c` | **修改** | 添加 WS63 通知 "可以睡眠了" 的命令处理 |

### 3.2 新增函数

#### a) `esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1)`

- 使用 EXT0 唤醒源
- GPIO2 **高电平**（`1`）唤醒
- WS63 拉高 GPIO2 → ESP32 从 deep sleep 唤醒

#### b) `void system_enter_deep_sleep(void)`

```c
// 伪代码
void system_enter_deep_sleep(void) {
    ESP_LOGI(TAG, "Entering deep sleep...");
    
    // 1. 关闭外设
    camera_module_deinit();
    led_indicator_deinit();
    
    // 2. 配置唤醒源: GPIO2 高电平唤醒
    esp_deep_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1);
    
    // 3. 停用所有 WDT (睡眠前必须)
    // 通知所有任务停止喂狗
    
    // 4. 进入 deep sleep
    esp_deep_sleep_start();
    // 唤醒后会从 app_main 重新启动
}
```

### 3.3 唤醒后恢复流程

```
ESP32 上电/唤醒 → app_main()
    │
    ├─ nvs_flash_init()
    ├─ init_uart()
    ├─ led_indicator_init()
    ├─ 创建队列和互斥锁
    ├─ protocol_handler_init()
    │   └─ ws63_recv_task 创建
    └─ 创建任务 (uart_task / camera_ai_task / ...)
        │
        └─ 等待 WS63 下发命令 (正常业务流程)
```

Deep sleep 唤醒等效于复位，**应用代码无需特殊处理**。

### 3.4 空闲检测机制

在 `app_main()` 的 while 循环中检测空闲状态：

```c
// app_main 最后
while (1) {
    // 检查是否可以睡眠
    if (g_ws63_state == WS63_STATE_IDLE && g_camera_power_on == false) {
        // 检查 GPIO2 电平: WS63 是否允许睡眠
        if (gpio_get_level(GPIO_NUM_2) == 0) {
            // 空闲超过 10 秒且 WS63 允许睡眠
            static int idle_count = 0;
            idle_count++;
            if (idle_count >= 10) {  // 10秒
                system_enter_deep_sleep();
            }
        } else {
            idle_count = 0;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

### 3.5 WS63 控制指令（可选）

在现有协议中扩展，可以让 WS63 显式通知 ESP32 可以睡眠或强制唤醒：

**WS63 → ESP32**（新增命令）：
```json
{"cmd": "sleep"}
```

ESP32 收到后立即进入 deep sleep（不需等待空闲检测）。

**现有机制**：WS63 通过 GPIO2 电平控制即可，不依赖 UART 命令。

---

## 4. 需修改的 sdkconfig 选项

| 配置项 | 当前值 | 目标值 | 说明 |
|--------|-------|-------|------|
| `CONFIG_ESP_TASK_WDT` | y | y (保留) | 睡眠前需解注册所有任务 |
| `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS` | y | y (已有) | 已启用 |
| `CONFIG_PM_SLP_DISABLE_PSRAM` | - | 需添加 | 睡眠前禁用 PSRAM |

---

## 5. 实现步骤（按优先级）

| 步骤 | 内容 | 工作量 | 依赖 |
|------|------|--------|------|
| 1 | `main.h` 声明 `system_enter_deep_sleep()` | 5 行 | 无 |
| 2 | `main.c` 实现睡眠函数 + 空闲检测 | ~30 行 | 步骤 1 |
| 3 | `main.c` 中 `gpio_config()` 初始化 GPIO2 为输入 | ~10 行 | 无 |
| 4 | 测试：拉高/拉低 GPIO2 验证唤醒 | 硬件测试 | 步骤 1-3 |
| 5 | 可选：`ws63_recv_task` 在 UI 菜单中打印 "Sleep" 选项 | ~10 行 | 步骤 4 |

---

## 6. 注意事项

### 6.1 PSRAM 与 Deep Sleep 兼容性

ESP32-S3 配置了 8MB PSRAM。进入 deep sleep 前需注意：
- PSRAM 数据在睡眠期间**全部丢失**
- 唤醒后重新初始化 PSRAM（`app_main` 自动处理）
- 需要 `CONFIG_PM_SLP_DISABLE_PSRAM=y` 确保睡眠前正确下电

### 6.2 GPIO2 电平约束

- **唤醒电平必须保持足够长**：ESP32 检测到 GPIO2 高电平时开始唤醒流程（~60ms）
- **WS63 拉高时机**：必须在发送 UART 命令**之前**先拉高 GPIO2，给 ESP32 留出启动时间

### 6.3 Task Watchdog 处理

Deep sleep 前必须解注册所有任务的 WDT：
- `esp_task_wdt_deinit()` 全局关闭 TWDT
- 或逐个 `esp_task_wdt_delete(NULL)`（每个任务自己调用）

最简单方案：在 `system_enter_deep_sleep()` 中调用 `esp_task_wdt_deinit()` 关闭 WDT，然后 `esp_deep_sleep_start()` 立即生效。

### 6.4 功耗估算

| 模式 | 电流 | 说明 |
|------|------|------|
| 正常全速 | ~200-300mA | 含摄像头+PSRAM+AI |
| 空闲无任务 | ~80-150mA | 无摄像头，仅 UART 轮询 |
| Deep sleep | ~5-10µA | RTC 定时器+GPIO 唤醒 |

Deep sleep 可实现 **降低 99.9% 待机功耗**，适合电池供电场景。