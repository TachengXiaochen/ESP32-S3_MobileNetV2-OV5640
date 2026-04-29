# ESP32-S3 ↔ WS63 通信协议实施计划

> **状态**: ✅ **已完成** (V3.0)  
> **实施日期**: 2026-04-29  
> **协议版本**: v1.0  

基于你的反馈，方案已收敛如下：

---

## ✅ 实施状态

**所有功能已完成实现**，详见 [docs/WS63_ESP32_PROTOCOL.md](docs/WS63_ESP32_PROTOCOL.md)

### 已实现功能清单

- ✅ UART1 通信（GPIO17/18, 115200 baud）
- ✅ JSON Lines 帧格式解析
- ✅ 完整的下行命令处理（register/inventory/outbound/capture/delete/cancel/list_assets/get_asset/sys_info/ping）
- ✅ 完整的上行消息推送（capture_progress/task_done/asset_list/asset_detail/sys_info/pong/error）
- ✅ 分步交互模式（硬件初始化与拍摄分离）
- ✅ 实时进度上报
- ✅ 状态机管理（5种状态）
- ✅ 错误码体系（17种错误码）
- ✅ 看门狗保护
- ✅ 异步非阻塞接收任务
- ✅ 双模式并行（UART0调试 + UART1 WS63通信）

### 代码模块

- **协议处理器**: [main/protocol_handler.c](main/protocol_handler.c) / [main/protocol_handler.h](main/protocol_handler.h)
- **完整协议规范**: [docs/WS63_ESP32_PROTOCOL.md](docs/WS63_ESP32_PROTOCOL.md)

---

## 一、硬件连接

| 信号 | ESP32-S3 | WS63 | 说明 |
|------|----------|------|------|
| UART TX | **GPIO17** | RX | cJSON 数据发送 |
| UART RX | **GPIO18** | TX | cJSON 命令接收 |
| RTC 唤醒 | **GPIO2** | IO | WS63 拉高 → ESP32 从 deep sleep 唤醒 |
| UART0 | TX=GPIO43, RX=GPIO44 | PC(USB) | **保留**，仅用于调试日志/CLI |

---

### 二、交互模型

```
        WS63 (主控)                              ESP32-S3 (感知节点)
        ──────────                                ──────────────────
                                                       
  ① GPIO2 拉高唤醒 ──────────────────────────────► 唤醒并启动
                                                     
  ② {"cmd":"register","mac":"AA:BB",...} ────────► 解析命令，初始化camera+storage
                                                     
                                                  ③ 自动开始拍摄front
                                                     ◄── {"type":"capture_progress","view":"front","status":"ok",...}
                                                     
                                                  ④ 自动拍摄side
                                                     ◄── {"type":"capture_progress","view":"side","status":"ok",...}
                                                     
                                                  ⑤ 自动拍摄top → 推理 → 保存
                                                     ◄── {"type":"task_done","task":"register","mac":"...","result":"success",...}
                                                     
  ⑥ GPIO2 拉低（或保持）◄──────────────────────── 进入低功耗等待
```

**核心原则**：
- WS63 下发一条完整命令（含所有参数），ESP32 自动完成整个流程
- 每个视图拍摄完成后**实时异步推送**进度给 WS63
- 全部完成后**主动推送**最终结果
- 无需 WS63 逐步确认，符合你"ESP32掌握数据权"的设计

---

### 三、完整协议定义

#### 3.1 下行命令（WS63 → ESP32）

```jsonc
// ========== 入库注册 ==========
{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}

// ========== 盘点比对 ==========
{"cmd":"inventory","mac":"AA:BB:CC:DD:EE:FF"}

// ========== 出库核验 ==========
{"cmd":"outbound","mac":"AA:BB:CC:DD:EE:FF","remove_qty":10}

// ========== 删除资产 ==========
{"cmd":"delete","mac":"AA:BB:CC:DD:EE:FF"}

// ========== 查询所有资产列表 ==========
{"cmd":"list_assets"}

// ========== 查询单个资产详情 ==========
{"cmd":"get_asset","mac":"XX:XX:XX:XX:XX:XX"}

// ========== 系统信息 ==========
{"cmd":"sys_info"}

// ========== 切换存储模式 ==========
{"cmd":"storage_mode","mode":"sd"}        // 或 "spiffs"

// ========== 取消当前任务 ==========
{"cmd":"cancel"}

// ========== 心跳/状态查询 ==========
{"cmd":"ping"}
```

#### 3.2 上行消息（ESP32 → WS63）

```jsonc
// ========== 心跳响应 ==========
{"type":"pong","camera_ready":true,"storage_ready":true,"free_heap":234567}

// ========== 拍摄进度 ==========
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"front","step":"1/3","status":"ok","blur_score":87.3}
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"side","step":"2/3","status":"ok","blur_score":91.2}
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"top","step":"3/3","status":"ok","blur_score":84.6}

// ========== 任务完成 - 入库 ==========
{"type":"task_done","task":"register","mac":"AA:BB:CC:DD:EE:FF","result":"success","item_name":"扳手","storage_area":"A","quantity":50,"file_size_kb":45}

// ========== 任务完成 - 入库失败（如超模糊） ==========
{"type":"task_done","task":"register","mac":"AA:BB:CC:DD:EE:FF","result":"failed","reason":"blur_detected","view":"front","blur_score":12.5}

// ========== 任务完成 - 盘点 ==========
{"type":"task_done","task":"inventory","mac":"AA:BB:CC:DD:EE:FF","result":"success","is_match":true,
  "weighted_confidence":0.892,"front_confidence":0.91,"side_confidence":0.88,"top_confidence":0.85,
  "threshold":0.75,"item_name":"扳手","storage_area":"A","quantity":50}

// ========== 任务完成 - 盘点(不匹配) ==========
{"type":"task_done","task":"inventory","mac":"AA:BB:CC:DD:EE:FF","result":"success","is_match":false,
  "weighted_confidence":0.321,"threshold":0.75}

// ========== 任务完成 - 出库 ==========
{"type":"task_done","task":"outbound","mac":"AA:BB:CC:DD:EE:FF","result":"success","is_match":true,
  "confidence":0.93,"original_qty":50,"remove_qty":10,"remaining_qty":40}

// ========== 任务完成 - 出库(库存清零，资产已删除) ==========
{"type":"task_done","task":"outbound","mac":"AA:BB:CC:DD:EE:FF","result":"success","is_match":true,
  "confidence":0.93,"original_qty":10,"remove_qty":10,"remaining_qty":0,"asset_deleted":true}

// ========== 任务完成 - 删除 ==========
{"type":"task_done","task":"delete","mac":"AA:BB:CC:DD:EE:FF","result":"success"}
{"type":"task_done","task":"delete","mac":"AA:BB:CC:DD:EE:FF","result":"failed","reason":"not_found"}

// ========== 资产列表响应 ==========
{"type":"asset_list","count":3,
  "assets":[
    {"mac":"AA:BB:CC:DD:EE:01","item_name":"扳手","storage_area":"A","quantity":50},
    {"mac":"AA:BB:CC:DD:EE:02","item_name":"螺丝刀","storage_area":"B","quantity":100},
    {"mac":"AA:BB:CC:DD:EE:03","item_name":"电钻","storage_area":"A","quantity":5}
  ]}

// ========== 单个资产详情 ==========
{"type":"asset_detail","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}

// ========== 资产不存在 ==========
{"type":"asset_detail","mac":"AA:BB:CC:DD:EE:FF","found":false}

// ========== 系统信息 ==========
{"type":"sys_info","free_heap":234567,"min_free_heap":210000,"camera_ready":true,
  "storage_ready":true,"storage_mode":"sd_card","storage_total_mb":32000,"storage_free_mb":15000}

// ========== 错误 ==========
{"type":"error","code":"INVALID_MAC","msg":"Invalid MAC address format"}
{"type":"error","code":"STORAGE_NOT_READY","msg":"SD card not mounted"}
{"type":"error","code":"CAMERA_FAIL","msg":"Camera initialization failed"}
{"type":"error","code":"ASSET_NOT_FOUND","msg":"Asset not found for outbound"}
{"type":"error","code":"UNKNOWN_CMD","msg":"Unknown command: xxx"}
```

---

### 四、cJSON 帧协议

采用 **JSON Lines** 格式（每行一个完整的 JSON对象，`\n` 结尾）：

```
下行: {"cmd":"register",...}\n
上行: {"type":"capture_progress",...}\n
```

- 解析时按 `\n` 分割帧
- 单帧 ≤ 2KB（资产列表最多 50 条仍轻松容纳）
- 使用 `cJSON_Parse()` 解析，`cJSON_PrintUnformatted()` 生成

---

### 五、任务流程时序（分步控制，B方案）

以**入库**为例：

```
WS63                     ESP32
 │  {"cmd":"register","mac":"AA:BB","item_name":"扳手","storage_area":"A","quantity":50}
 ├────────────────────────►│
 │                         │ ① 解析cJSON → 验证参数
 │                         │ ② 初始化camera + storage + AI
 │                         │ ③ 拍摄front → 推理 → 保存JPEG
 │  {"type":"capture_progress","view":"front","step":"1/3","status":"ok","blur_score":87.3}
 │◄────────────────────────┤
 │                         │ ④ 拍摄side → 推理
 │  {"type":"capture_progress","view":"side","step":"2/3","status":"ok","blur_score":91.2}
 │◄────────────────────────┤
 │                         │ ⑤ 拍摄top → 推理 → 3视图融合 → 保存资产记录
 │  {"type":"capture_progress","view":"top","step":"3/3","status":"ok","blur_score":84.6}
 │◄────────────────────────┤
 │  {"type":"task_done","task":"register","mac":"AA:BB",...}
 │◄────────────────────────┤
 │                         │ ⑥ 关闭camera → 回到等待状态
```

**盘点/出库同理**，差异仅在视图数量（盘点=3视图，出库=1视图）和最终结果字段。

---

### 六、文件结构规划

```
main/
├── main.c                    # 改造：新增 ws63_uart_task, 保留 UART0 调试, 新增 GPIO2 RTC唤醒
├── main.h                    # 改造：新增 WS63 相关宏和变量声明
├── cmd_handler.h/c           # 改造：适配 cJSON 输入源，输出也改为回调构造 cJSON
├── protocol_handler.h        # 新增：cJSON 协议处理模块
├── protocol_handler.c        # 新增：
│   ├── ws63_send_json()         # 构造并发送 cJSON → UART1
│   ├── ws63_recv_task()         # UART1 接收 → 按\n分割 → cJSON解析 → 分发
│   ├── handle_register()        # 入库cJSON命令处理
│   ├── handle_inventory()       # 盘点cJSON命令处理
│   ├── handle_outbound()        # 出库cJSON命令处理
│   ├── handle_delete()          # 删除cJSON命令处理
│   ├── handle_list_assets()     # 资产列表查询
│   ├── handle_get_asset()       # 单资产查询
│   ├── handle_sys_info()        # 系统信息查询
│   ├── handle_ping()            # 心跳
│   ├── handle_cancel()          # 取消任务
│   └── ws63_report_progress()   # 拍摄进度上报
│   └── ws63_report_task_done()  # 任务完成上报
├── camera_module.h/c         # 不变
├── ai_module.h/c             # 不变
├── storage_module.h/c        # 不变
├── asset_manager.h/c         # 不变
├── feature_processor.h/c     # 不变
├── similarity_matcher.h/c    # 不变
├── blur_detection.h/c        # 不变
├── led_indicator.h/c         # 不变
└── CMakeLists.txt            # 改造：添加 protocol_handler.c
```

---

### 七、GPIO2 RTC 唤醒逻辑

```c
#define WS63_WAKEUP_GPIO    GPIO_NUM_2

// ESP32 启动后配置 GPIO2 为 RTC 唤醒源
// WS63 拉高 GPIO2 → ESP32 从 deep_sleep 唤醒 → 初始化 UART1 → 等待 WS63 下发命令
// 任务完成后 → ESP32 通过 UART 发送 {"type":"ready_to_sleep"} → WS63 拉低 GPIO2 → ESP32 进入 deep_sleep
```

简化版（当前阶段暂不实现 deep_sleep，仅预留 GPIO2 状态读取）：
```c
// 初期：GPIO2 作为输入，WS63 拉高表示"准备通讯"，ESP32 在空闲时读取该引脚状态
// 后期：接入 RTC GPIO 做 deep_sleep 唤醒
```

---

### 八、与现有 CLI（UART0）的关系

| 功能 | UART0 (GPIO43/44) | UART1 (GPIO17/18) |
|------|-------------------|-------------------|
| 用途 | **调试**：LOG输出、help/?、exit等 | **生产**：WS63 cJSON 命令/响应 |
| 人机交互 | ✅ 保持不变（cmd_handler） | ❌ 无 |
| 业务命令 | ⚠️ 可保留 `r/c/o/d/l/i` 用于PC调试 | ✅ 全部 cJSON 协议命令 |
| 输出格式 | 人类可读文本 | 结构化 JSON |

---

### 九、JPEG 传输预留接口

```c
// protocol_handler.h 中预留
/**
 * @brief 预留：通过 UART1 传输 JPEG 图像数据给 WS63
 * @param mac       MAC地址
 * @param view_name 视图名称
 * @return ESP_OK if image exists and sent
 * @note  当前阶段空实现（返回 ESP_ERR_NOT_SUPPORTED）
 *        后期星闪带宽确认后，实现分片传输（基于 cJSON base64 或二进制帧）
 */
esp_err_t ws63_send_jpeg(const char *mac, const char *view_name);
```

---

### 十、实现优先级

| 优先级 | 模块 | 说明 |
|-------|------|------|
| P0 | `protocol_handler.h/c` | cJSON 编解码核心 |
| P0 | `main.c` UART1 接收任务 | 替换/并行 uart_task |
| P0 | `main.c` 进度/结果回调 | capture_progress + task_done 上报 |
| P0 | 下行6条核心命令 | register/inventory/outbound/delete/list_assets/sys_info |
| P1 | cancel / ping / get_asset / storage_mode | 控制类命令 |
| P2 | GPIO2 RTC 唤醒 | deep_sleep 低功耗 |
| P3 | JPEG 分片传输 | 星闪带宽确认后 |

---
