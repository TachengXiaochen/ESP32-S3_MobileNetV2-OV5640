# WS63 ↔ ESP32-S3 通信协议规范

> **文档版本**: v2.0  
> **最后更新**: 2026-05-09  
> **适用项目**: CAM_AI (ESP32-S3 视觉感知物资管理子节点)  

---

## 目录

1. [系统架构](#1-系统架构)
2. [硬件连接](#2-硬件连接)
3. [物理层规范](#3-物理层规范)
4. [帧格式](#4-帧格式)
5. [通信模型](#5-通信模型)
6. [下行命令 WS63 → ESP32](#6-下行命令-ws63--esp32)
7. [上行消息 ESP32 → WS63](#7-上行消息-esp32--ws63)
8. [任务时序流程](#8-任务时序流程)
9. [错误码定义](#9-错误码定义)
10. [状态机](#10-状态机)
11. [预留接口](#11-预留接口)
12. [附录](#12-附录)
13. [L610](#13-4g通道扩展l610)

---

## 1. 系统架构

```
┌───────────────────────┐         UART (cJSON)        ┌──────────────────────────┐
│        WS63           │ ◄──────────────────────────► │       ESP32-S3           │
│     (主控/Host)        │   TX ───────────────► RX    │    (视觉感知子节点)        │
│                       │   RX ◄─────────────── TX    │                          │
│  职责：                │   GPIO2 ────────────► RTC   │  职责：                   │
│  · 星闪网络全局时钟     │    (唤醒信号)               │  · 3视图拍摄 (前/侧/顶)   │
│  · 人机交互/UI         │                             │  · MobileNet 特征提取     │
│  · 业务逻辑编排         │   UART0 (调试保留)          │  · 多帧融合 + 模糊检测    │
│  · 云平台通讯           │   TX/RX ───────────► PC    │  · 相似度匹配 + 盘点比对  │
│  · 指令下发 + 结果收集  │                             │  · SD卡资产存储/管理       │
└───────────────────────┘                             └──────────────────────────┘
```

**核心原则：**
- WS63 掌握**控制权**（何时让 ESP32 工作）
- ESP32 掌握**数据权**（推理完成主动推送结果，无需 WS63 轮询）

---

## 2. 硬件连接

| 信号 | ESP32-S3 引脚 | WS63 引脚 | 方向 | 说明 |
|------|-------------|----------|------|------|
| UART TX | **GPIO17** | RX | ESP32 → WS63 | cJSON 数据发送 |
| UART RX | **GPIO18** | TX | WS63 → ESP32 | cJSON 命令接收 |
| RTC 唤醒 | **GPIO2** | GPIO (推挽输出) | WS63 → ESP32 | 拉高唤醒 ESP32，拉低允许睡眠 |
| GND | GND | GND | — | 共地 |

> **调试接口（保留，不变）**：UART0 使用默认引脚 TX=GPIO43 / RX=GPIO44，连接 PC 用于开发调试和日志输出。

---

## 3. 物理层规范

| 参数 | 值 |
|------|-----|
| 接口 | UART (全双工) |
| 波特率 | **115200** bps |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | 无 |
| 流控 | 无 (仅 TX/RX 两线) |
| 逻辑电平 | 3.3V LVTTL |

---

## 4. 帧格式

### 4.1 基本帧

采用 **JSON Lines** 格式（每行一个完整的 JSON 对象，以 `\n` 结尾）：

```
┌─────────────────────────────────────┬──────┐
│          JSON Body (UTF-8)          │  \n  │
│  {"type":"task_done","task":"...",...}   │  0x0A │
└─────────────────────────────────────┴──────┘
```

### 4.2 帧规则

| 规则 | 说明 |
|------|------|
| 编码 | UTF-8，无 BOM |
| 分隔符 | 每帧以 `\n` (0x0A) 结束 |
| 帧内禁止换行 | JSON 对象内部不得包含 `\n`，使用 `cJSON_PrintUnformatted()` 生成 |
| 最大帧长 | ≤ **2048 字节** (资产列表最多 50 条时仍远小于此值) |
| 解析方式 | 按 `\n` 分割帧 → `cJSON_Parse()` 解析 JSON |
| 空白帧忽略 | 收到空行或纯空白行直接丢弃 |

### 4.3 示例帧

```
下行命令:
{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}\n

上行消息:
{"type":"task_done","task":"register","mac":"AA:BB:CC:DD:EE:FF","result":"success","item_name":"扳手","storage_area":"A","quantity":50,"file_size_kb":45}\n
```

---

## 5. 通信模型

### 5.1 双向异步模式

```
WS63 (主控方)                          ESP32-S3 (感知方)
───────────                            ───────────────

[1] GPIO2 拉高唤醒                     → 唤醒启动，初始化外设

[2] 下发一条完整命令 (cmd)              → 收到命令，开始执行任务
    例: {"cmd":"register","mac":"...",...}

[3] 可下发 cancel 中断当前任务          → 收到 cancel，安全终止

                                      [4] 拍摄每个视图后，主动推送进度
                                      ← {"type":"capture_progress",...}

                                      [5] 任务全部完成后，主动推送结果
                                      ← {"type":"task_done",...}

[6] GPIO2 拉低 (或下发 sleep 心跳)      → 进入低功耗等待
```

**关键约束：**

1. WS63 一次只能下发**一条业务命令**，ESP32 在任务执行期间忽略新下发的业务命令（`cancel` 除外）
2. WS63 可随时下发查询命令（`list_assets`、`sys_info`、`ping`），即使 ESP32 正在执行任务
3. ESP32 不主动发起任何业务请求，仅**响应命令**和**推送任务进度/结果**

### 5.2 命令与消息类型总览

| 类别 | 方向 | 字段标识 | 对应现有功能 | 说明 |
|------|------|---------|-------------|------|
| 业务命令 | 下行 | `"cmd":"register"` | CLI `r` 流程 | 入库注册（仅初始化） |
| 业务命令 | 下行 | `"cmd":"inventory"` | CLI `c` 流程 | 盘点比对（仅初始化） |
| 业务命令 | 下行 | `"cmd":"outbound"` | CLI `o` 流程 | 出库核验（仅初始化） |
| 业务命令 | 下行 | `"cmd":"capture"` | CLI `f`/`s`/`t` 按键 | 单步拍摄视图 |
| 业务命令 | 下行 | `"cmd":"delete"` | CLI `d` 流程 | 删除资产 |
| 控制命令 | 下行 | `"cmd":"cancel"` | 复用 `exit` 清理逻辑 | 取消当前任务 |
| 查询命令 | 下行 | `"cmd":"list_assets"` | `asset_list_uart()` | 查询资产列表 |
| 查询命令 | 下行 | `"cmd":"get_asset"` | `asset_load()` 封装 | 查询单个资产详情 |
| 查询命令 | 下行 | `"cmd":"sys_info"` | `print_system_info_uart()` | 查询系统信息 |
| 查询命令 | 下行 | `"cmd":"ping"` | 新增 | 心跳/状态检测 |
| 进度上报 | 上行 | `"type":"capture_progress"` | — | 拍摄进度 |
| 结果上报 | 上行 | `"type":"task_done"` | — | 任务完成结果 |
| 查询响应 | 上行 | `"type":"asset_list"` | — | 资产列表 |
| 查询响应 | 上行 | `"type":"asset_detail"` | — | 单资产详情 |
| 查询响应 | 上行 | `"type":"sys_info"` | — | 系统信息 |
| 查询响应 | 上行 | `"type":"pong"` | — | 心跳响应 |
| 异常上报 | 上行 | `"type":"error"` | — | 错误报告 |

---

## 6. 下行命令 (WS63 → ESP32)

### 6.1 入库注册

```json
{
  "cmd": "register",
  "mac": "AA:BB:CC:DD:EE:FF",
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"register"` |
| `mac` | string | 是 | MAC 地址，格式 `XX:XX:XX:XX:XX:XX` (17字符) |
| `item_name` | string | 是 | 物品名称，1-127 字符 |
| `storage_area` | string | 是 | 存放区域，单个大写字母 A-Z |
| `quantity` | uint32 | 是 | 入库数量，正整数 (≥1) |

> ⚠️ **分步交互说明**：此命令仅验证参数并初始化硬件（camera + storage + AI），完成后返回 `capture_progress(step:"0/3", status:"ready")`。拍摄由 WS63 通过 `capture` 命令逐个控制。

**ESP32 执行流程：**  
数据验证 → 保存注册信息 → 初始化 camera + storage + AI → 进入 WAITING_CAPTURE 状态

**上行响应序列：**
```
capture_progress (view="none", step="0/3", status="ready")   ← 初始化完成
  ...（WS63 发送 capture ×3，见 §8.1 时序图）...
task_done (task="register", result="success"|"failed")
```

---

### 6.2 盘点比对

```json
{
  "cmd": "inventory",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"inventory"` |
| `mac` | string | 是 | 要盘点的资产 MAC 地址 |

**前置条件：** 该 MAC 必须已注册，否则返回 error `ASSET_NOT_FOUND`

> ⚠️ **分步交互说明**：此命令仅加载参考特征并初始化硬件，完成后返回 `capture_progress(step:"0/3", status:"ready")`。拍摄由 WS63 通过 `capture` 命令逐个控制。

**ESP32 执行流程：**  
加载参考特征(从SD卡) → 初始化 camera + AI → 进入 WAITING_CAPTURE 状态

**上行响应序列：**
```
capture_progress (view="none", step="0/3", status="ready")   ← 初始化完成
  ...（WS63 发送 capture ×3，见 §8.2 时序图）...
task_done (task="inventory", result="success", is_match=true/false, ...)
```

---

### 6.3 出库核验

```json
{
  "cmd": "outbound",
  "mac": "AA:BB:CC:DD:EE:FF",
  "remove_qty": 10
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"outbound"` |
| `mac` | string | 是 | 要出库的资产 MAC 地址 |
| `remove_qty` | uint32 | 是 | 出库数量，正整数 (≥1) |

**前置条件：** 该 MAC 必须已注册，否则返回 error `ASSET_NOT_FOUND`

> ⚠️ **分步交互说明**：此命令仅验证资产存在并初始化硬件，完成后返回 `capture_progress(step:"0/1", status:"ready")`。拍摄由 WS63 通过 `capture` 命令控制。

**ESP32 执行流程：**  
加载参考特征 → 验证资产存在 → 初始化 camera + AI → 进入 WAITING_CAPTURE 状态

**上行响应序列：**
```
capture_progress (view="none", step="0/1", status="ready")   ← 初始化完成
  ...（WS63 发送 capture ×1，见 §8.3 时序图）...
task_done (task="outbound", result="success"|"failed", is_match=true|false, ...)
```

---

### 6.4 单步拍摄视图

```json
{"cmd":"capture","view":"front"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"capture"` |
| `view` | string | 是 | `"front"` / `"side"` / `"top"` |

**前置条件：** 必须先收到 `register` / `inventory` / `outbound` 命令完成硬件初始化，否则返回 error `NOT_INITIALIZED`。

**ESP32 执行流程：** 拍摄指定视图 → 模糊检测 → 推理(3帧融合) → 返回 `capture_progress`

**上行响应：**
```
capture_progress (view=..., step=..., status=ok|failed)
```

> **最后一个视图拍摄完成后**（如入库/盘点的第3个 `capture` 或出库的第1个），ESP32 自动触发最终推理融合+保存，返回 `task_done`。
>
> 对应现有 CLI 中的 `f` / `s` / `t` 按键，WS63 可完全控制拍摄节奏，例如在机械臂到位后再发下一个 `capture`。

---

### 6.5 删除资产

```json
{
  "cmd": "delete",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"delete"` |
| `mac` | string | 是 | 要删除的资产 MAC 地址 |

> **注意：** WS63 应在发送此命令前自行完成用户确认，ESP32 收到后**直接执行删除**，不再二次确认。

**上行响应：**
```
task_done (task="delete", result="success"|"failed")
```

---

### 6.6 取消当前任务

```json
{
  "cmd": "cancel"
}
```

立即中断正在执行的任务（入库/盘点/出库），关闭摄像头，重置状态。

**上行响应：**
```
task_done (task="<原任务>", result="cancelled")
```

---

### 6.7 查询资产列表

```json
{
  "cmd": "list_assets"
}
```

**上行响应：**
```
asset_list (count=N, assets=[...])
```

---

### 6.8 查询单个资产详情

```json
{
  "cmd": "get_asset",
  "mac": "AA:BB:CC:DD:EE:FF"
}
```

**上行响应：**
```
asset_detail (found=true|false, mac=..., item_name=..., ...)
```

---

### 6.9 查询系统信息

```json
{
  "cmd": "sys_info"
}
```

**上行响应：**
```
sys_info (free_heap=..., camera_ready=..., ...)
```

---

### 6.10 心跳/状态检测

```json
{
  "cmd": "ping"
}
```

**上行响应：**
```
pong (camera_ready=..., storage_ready=..., ...)
```

---

## 7. 上行消息 (ESP32 → WS63)

### 7.1 拍摄进度

```json
{
  "type": "capture_progress",
  "mac": "AA:BB:CC:DD:EE:FF",
  "view": "front",
  "step": "1/3",
  "status": "ok",
  "blur_score": 87.3,
  "feature_size": 1280
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"capture_progress"` |
| `mac` | string | 当前操作的 MAC 地址 |
| `view` | string | `"front"` / `"side"` / `"top"` |
| `step` | string | 当前步骤，如 `"1/3"` (入库/盘点) 或 `"1/1"` (出库) |
| `status` | string | `"ok"` 或 `"blur_warning"` 或 `"failed"` |
| `blur_score` | float | 模糊度得分 (拉普拉斯方差)，值越大越清晰，通常 > 50 为合格 |
| `feature_size` | int | 特征向量维度 (固定 1280) |

---

### 7.2 任务完成 — 入库

```json
{
  "type": "task_done",
  "task": "register",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success",
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50,
  "is_overwrite": false,
  "file_size_kb": 45
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"task_done"` |
| `task` | string | `"register"` |
| `result` | string | `"success"` / `"failed"` |
| `mac` | string | 资产 MAC 地址 |
| `item_name` | string | 物品名称 |
| `storage_area` | string | 存放区域 |
| `quantity` | uint32 | 入库数量 |
| `is_overwrite` | bool | 是否为覆盖已有资产 (true=更新, false=新创建) |
| `file_size_kb` | int | JPEG 文件总大小 (3视图合计, KB) |

---

### 7.3 任务完成 — 盘点

```json
{
  "type": "task_done",
  "task": "inventory",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success",
  "is_match": true,
  "weighted_confidence": 0.892,
  "front_confidence": 0.91,
  "side_confidence": 0.88,
  "top_confidence": 0.85,
  "threshold": 0.75,
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"task_done"` |
| `task` | string | `"inventory"` |
| `result` | string | `"success"` / `"failed"` |
| `is_match` | bool | 是否匹配 (weighted_confidence ≥ threshold) |
| `weighted_confidence` | float | 加权置信度 (0.5×front + 0.3×side + 0.2×top) |
| `front_confidence` | float | 正视图置信度 |
| `side_confidence` | float | 侧视图置信度 |
| `top_confidence` | float | 俯视图置信度 |
| `threshold` | float | 动态匹配阈值 |
| `item_name` | string | 物品名称 |
| `storage_area` | string | 存放区域 |
| `quantity` | uint32 | 当前库存数量 |

> 当 `result="failed"` 时不包含置信度等详情字段，改为包含 `reason` 字段说明失败原因。

---

### 7.4 任务完成 — 出库

```json
{
  "type": "task_done",
  "task": "outbound",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success",
  "is_match": true,
  "confidence": 0.93,
  "threshold": 0.75,
  "item_name": "扳手",
  "original_qty": 50,
  "remove_qty": 10,
  "remaining_qty": 40,
  "asset_deleted": false
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"task_done"` |
| `task` | string | `"outbound"` |
| `result` | string | `"success"` / `"failed"` / `"cancelled"` |
| `is_match` | bool | 正视图是否匹配参考特征 |
| `confidence` | float | 匹配置信度 |
| `threshold` | float | 匹配阈值 |
| `item_name` | string | 物品名称 |
| `original_qty` | uint32 | 原始库存数量 |
| `remove_qty` | uint32 | 出库数量 |
| `remaining_qty` | uint32 | 剩余库存数量 (0 表示库存已清空) |
| `asset_deleted` | bool | 库存扣至0后是否已删除资产记录 |

---

### 7.5 任务完成 — 删除

```json
{
  "type": "task_done",
  "task": "delete",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"task_done"` |
| `task` | string | `"delete"` |
| `mac` | string | 被删除的资产 MAC 地址 |
| `result` | string | `"success"` / `"failed"` |

---

### 7.6 任务完成 — 取消

```json
{
  "type": "task_done",
  "task": "register",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "cancelled"
}
```

---

### 7.7 资产列表

```json
{
  "type": "asset_list",
  "count": 3,
  "assets": [
    {
      "mac": "AA:BB:CC:DD:EE:01",
      "item_name": "扳手",
      "storage_area": "A",
      "quantity": 50
    },
    {
      "mac": "AA:BB:CC:DD:EE:02",
      "item_name": "螺丝刀",
      "storage_area": "B",
      "quantity": 100
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"asset_list"` |
| `count` | int | 资产总数 |
| `assets` | array | 资产摘要数组 (仅含关键字段，不含特征向量) |
| `assets[n].mac` | string | MAC 地址 |
| `assets[n].item_name` | string | 物品名称 |
| `assets[n].storage_area` | string | 存放区域 |
| `assets[n].quantity` | uint32 | 库存数量 |

---

### 7.8 单个资产详情

```json
{
  "type": "asset_detail",
  "mac": "AA:BB:CC:DD:EE:FF",
  "found": true,
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50
}
```

资产不存在时：
```json
{
  "type": "asset_detail",
  "mac": "AA:BB:CC:DD:EE:FF",
  "found": false
}
```

---

### 7.9 系统信息

```json
{
  "type": "sys_info",
  "free_heap": 234567,
  "min_free_heap": 210000,
  "camera_ready": true,
  "storage_ready": true,
  "storage_mode": "sd_card",
  "storage_total_mb": 32000,
  "storage_free_mb": 15000,
  "current_task": "idle"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"sys_info"` |
| `free_heap` | uint32 | 当前可用堆内存 (字节) |
| `min_free_heap` | uint32 | 历史最小可用堆内存 (字节) |
| `camera_ready` | bool | 摄像头是否已初始化 |
| `storage_ready` | bool | 存储是否已挂载 |
| `storage_mode` | string | 固定值 `"sd_card"` (仅支持TF卡) |
| `storage_total_mb` | uint32 | 存储总容量 (MB) |
| `storage_free_mb` | uint32 | 存储剩余容量 (MB) |
| `current_task` | string | 当前任务状态: `"idle"` / `"register"` / `"inventory"` / `"outbound"` |

---

### 7.10 心跳响应

```json
{
  "type": "pong",
  "camera_ready": true,
  "storage_ready": true,
  "free_heap": 234567,
  "current_task": "idle"
}
```

---

### 7.11 错误报告

```json
{
  "type": "error",
  "code": "INVALID_MAC",
  "msg": "Invalid MAC address format"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"error"` |
| `code` | string | 错误码 (见 [§9 错误码定义](#9-错误码定义)) |
| `msg` | string | 人类可读的错误描述 |

---

## 8. 任务时序流程

> **分步交互模式**：`register` / `inventory` / `outbound` 仅负责初始化硬件，WS63 通过 `capture` 命令逐个控制拍摄节奏。最后一个视图完成后 ESP32 自动触发最终推理并返回 `task_done`。

### 8.1 入库注册 (3视图，分步)

```
WS63                                 ESP32-S3
 │                                      │
 │  {"cmd":"register","mac":"AA:BB",    │
 │   "item_name":"扳手","storage_area":  │
 │   "A","quantity":50}                 │
 ├─────────────────────────────────────►│
 │                                      │ ① 验证参数，保存注册信息到全局变量
 │                                      │ ② 初始化 camera + storage + AI
 │                                      │ ③ 进入 WAITING_CAPTURE 状态
 │  {"type":"capture_progress",         │    (不拍摄，只返回初始化完成)
 │   "mac":"AA:BB","view":"none",       │
 │   "step":"0/3","status":"ready"}     │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"front"}    │  ← WS63 主动下发拍摄指令
 ├─────────────────────────────────────►│
 │                                      │ ④ 拍摄FRONT → 模糊检测 → 推理(3帧融合)
 │  {"type":"capture_progress",         │
 │   "mac":"AA:BB","view":"front",      │
 │   "step":"1/3","status":"ok",        │
 │   "blur_score":87.3}                 │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"side"}     │  ← WS63 主动下发
 ├─────────────────────────────────────►│
 │                                      │ ⑤ 拍摄SIDE → 推理
 │  {"type":"capture_progress",         │
 │   "mac":"AA:BB","view":"side",       │
 │   "step":"2/3","status":"ok",        │
 │   "blur_score":91.2}                 │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"top"}      │  ← 最后一个视图: 自动触发融合+保存
 ├─────────────────────────────────────►│
 │                                      │ ⑥ 拍摄TOP → 推理 → 三视图融合
 │  {"type":"capture_progress",         │
 │   "mac":"AA:BB","view":"top",        │
 │   "step":"3/3","status":"ok",        │
 │   "blur_score":84.6}                 │
 │◄─────────────────────────────────────┤
 │                                      │ ⑦ 保存资产记录 + JPEG
 │  {"type":"task_done",                │
 │   "task":"register","result":        │
 │   "success","mac":"AA:BB",           │
 │   "item_name":"扳手",...}             │
 │◄─────────────────────────────────────┤
 │                                      │ ⑧ 关闭camera → idle
```

### 8.2 盘点比对 (3视图，分步)

```
WS63                                 ESP32-S3
 │                                      │
 │  {"cmd":"inventory","mac":"AA:BB"}   │
 ├─────────────────────────────────────►│
 │                                      │ ① 加载参考特征(从SD卡)
 │                                      │ ② 初始化 camera + AI
 │                                      │ ③ 进入 WAITING_CAPTURE 状态
 │  {"type":"capture_progress",         │
 │   "mac":"AA:BB","view":"none",       │
 │   "step":"0/3","status":"ready"}     │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"front"}    │
 ├─────────────────────────────────────►│
 │                                      │ ④ 拍摄FRONT → 推理
 │  {"type":"capture_progress",         │
 │   "view":"front","step":"1/3","ok"}  │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"side"}     │
 ├─────────────────────────────────────►│
 │                                      │ ⑤ 拍摄SIDE → 推理
 │  {"type":"capture_progress",         │
 │   "view":"side","step":"2/3","ok"}   │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"top"}      │  ← 最后一个视图
 ├─────────────────────────────────────►│
 │                                      │ ⑥ 拍摄TOP → 推理
 │  {"type":"capture_progress",         │
 │   "view":"top","step":"3/3","ok"}    │
 │◄─────────────────────────────────────┤
 │                                      │ ⑦ 三视图特征 vs 参考特征匹配
 │  {"type":"task_done",                │
 │   "task":"inventory","result":       │
 │   "success","is_match":true,         │
 │   "weighted_confidence":0.892,...}   │
 │◄─────────────────────────────────────┤
 │                                      │ ⑧ 关闭camera → idle
```

### 8.3 出库核验 (1视图，分步)

```
WS63                                 ESP32-S3
 │                                      │
 │  {"cmd":"outbound","mac":"AA:BB",    │
 │   "remove_qty":10}                   │
 ├─────────────────────────────────────►│
 │                                      │ ① 加载参考特征, 验证资产存在
 │                                      │ ② 初始化 camera + AI
 │                                      │ ③ 进入 WAITING_CAPTURE 状态
 │  {"type":"capture_progress",         │
 │   "mac":"AA:BB","view":"none",       │
 │   "step":"0/1","status":"ready"}     │
 │◄─────────────────────────────────────┤
 │                                      │
 │  {"cmd":"capture","view":"front"}    │  ← 仅需1个视图, 拍摄完即触发结果
 ├─────────────────────────────────────►│
 │                                      │ ④ 拍摄FRONT → 推理
 │  {"type":"capture_progress",         │
 │   "view":"front","step":"1/1","ok"}  │
 │◄─────────────────────────────────────┤
 │                                      │ ⑤ 正视图 vs 参考特征匹配
 │                                      │ ⑥ 若匹配: 扣减数量
 │  {"type":"task_done",                │
 │   "task":"outbound","result":        │
 │   "success","is_match":true,         │
 │   "remaining_qty":40,...}            │
 │◄─────────────────────────────────────┤
 │                                      │ ⑦ 关闭camera → idle
```

### 8.4 取消任务 (任意时刻)

```
WS63                                 ESP32-S3
 │                                      │
 │  {"cmd":"cancel"}                    │  (正在执行 register 的第2步)
 ├─────────────────────────────────────►│
 │                                      │ ① 清空推理队列
 │                                      │ ② 关闭摄像头
 │                                      │ ③ 重置状态
 │  {"type":"task_done",                │
 │   "task":"register","result":        │
 │   "cancelled","mac":"AA:BB"}         │
 │◄─────────────────────────────────────┤
```

---

## 9. 错误码定义

| 错误码 | 说明 | 触发场景 |
|--------|------|---------|
| `INVALID_JSON` | JSON 解析失败 | 收到无法解析的帧 |
| `UNKNOWN_CMD` | 未知命令 | `cmd` 字段值不在支持列表中 |
| `MISSING_FIELD` | 缺少必填字段 | 命令缺少 `mac` / `item_name` 等必填字段 |
| `INVALID_MAC` | MAC 地址格式错误 | MAC 不符合 `XX:XX:XX:XX:XX:XX` 格式 |
| `INVALID_FIELD` | 字段值无效 | `quantity=0`、`storage_area` 非 A-Z 等 |
| `ASSET_NOT_FOUND` | 资产不存在 | 盘点/出库/删除时 MAC 未注册 |
| `ASSET_ALREADY_EXISTS` | 资产已存在 | (预留，当前自动覆盖) |
| `STORAGE_NOT_READY` | 存储未就绪 | SD 卡未挂载或初始化失败 |
| `CAMERA_FAIL` | 摄像头故障 | 摄像头初始化失败 |
| `AI_MODEL_FAIL` | AI 模型加载失败 | MobileNet 模型文件损坏或不存在 |
| `CAPTURE_FAIL` | 拍摄失败 | 摄像头采集超时或硬件错误 |
| `BLUR_DETECTED` | 图像模糊 | 拉普拉斯方差低于阈值 (默认 < 50)，任务中断 |
| `INFERENCE_FAIL` | 推理失败 | 特征提取或融合过程异常 |
| `SAVE_FAIL` | 保存失败 | SD 卡写入错误或空间不足 |
| `INTERNAL_ERROR` | 内部错误 | 内存分配失败等未预期异常 |
| `NOT_INITIALIZED` | 硬件未初始化 | 发送 `capture` 前未先发 `register`/`inventory`/`outbound` |
| `TASK_BUSY` | 任务忙 | 上一个任务未完成时收到新业务命令 |

---

## 10. 状态机

### 10.1 ESP32 全局状态

```
                    ┌──────────┐
        WS63 GPIO2  │   IDLE   │
        拉高唤醒     │  (空闲)   │
        ──────────► └────┬─────┘
                         │ 收到 cmd (register/inventory/outbound/delete)
                         ▼
                    ┌──────────┐
                    │  BUSY    │──── 收到 cancel ────► IDLE
                    │ (执行中)  │
                    └────┬─────┘
                         │ 任务完成 / 失败
                         ▼
                    ┌──────────┐
                    │  IDLE    │
                    └──────────┘
```

### 10.2 任务子状态 (以入库为例)

```
BUSY (register)
  ├── INIT        (初始化camera/storage/AI)
  ├── CAPTURE_F   (拍摄正视图)
  ├── CAPTURE_S   (拍摄侧视图)
  ├── CAPTURE_T   (拍摄俯视图)
  ├── INFERENCE   (三视图融合推理)
  ├── SAVING      (保存资产记录+JPEG)
  └── DONE        → IDLE
```

### 10.3 命令优先级

在 BUSY 状态下：

| 收到的命令 | 处理方式 |
|-----------|---------|
| `cancel` | **立即响应**，中止当前任务 |
| `ping` | **立即响应**，返回当前状态 |
| `sys_info` | **立即响应** |
| `list_assets` | **立即响应** (读SD卡, 可能较慢) |
| `get_asset` | **立即响应** |
| `register/inventory/outbound/delete` | **忽略**，返回 `{"type":"error","code":"TASK_BUSY"}` |

---

## 11. 预留接口

### 11.1 JPEG 图像传输 (预留)

> **当前状态**: 空实现，返回 `ESP_ERR_NOT_SUPPORTED`  
> **启用条件**: 星闪网络带宽确认后按需开发

```json
// WS63 请求下载某个视图的 JPEG
{"cmd":"get_jpeg","mac":"AA:BB:CC:DD:EE:FF","view":"front"}

// ESP32 响应 (方案A: base64 内嵌)
{"type":"jpeg_data","mac":"AA:BB:CC:DD:EE:FF","view":"front",
 "format":"base64","size_bytes":24576,
 "chunk":"1/5","data":"/9j/4AAQSkZJRgABAQAAAQABAAD..."}

// 或方案B: 二进制帧
// [0x02][4-byte length][binary JPEG data][CRC16] (待定)
```

### 11.2 远程固件升级 (预留)

```json
{"cmd":"ota_start","size_bytes":1048576,"md5":"abc123..."}
// 后接 OTA 二进制帧
{"cmd":"ota_cancel"}
```

### 11.3 摄像头参数远程配置 (预留)

```json
{"cmd":"camera_config","brightness":0,"contrast":0,"saturation":0,
 "quality":10,"framesize":"SVGA"}
```

---

## 12. 附录

### 12.1 MAC 地址格式规范

- 格式: `XX:XX:XX:XX:XX:XX`
- 长度: 17 字符 (含5个冒号)
- 字符集: `0-9`, `A-F`, `a-f` (大小写均可，内部统一转大写)
- 正则: `^[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}$`

### 12.2 特征向量说明

| 参数 | 值 |
|------|-----|
| 模型 | MobileNetV2 (或 V3) |
| 向量维度 | **1280** (float32) |
| 融合策略 | 3帧平均 → L2 归一化 → 温度缩放 |
| 相似度算法 | 加权融合: 0.7×Cosine + 0.3×(1-Normalized Euclidean) |
| 盘点权重 | Front=0.5, Side=0.3, Top=0.2 |
| 默认匹配阈值 | 0.75 (可通过 `asset_class` 动态调整) |

### 12.3 存储空间预留建议

| 项目 | 估算大小 |
|------|---------|
| 单条 asset_record (特征+元数据) | ~5.2KB (1280×4B×3 + 元数据) |
| 单视图 JPEG (SVGA, quality=10) | ~15-30KB |
| 一完整资产 (3视图JPEG+1条记录) | ~50-95KB |
| SD 卡推荐容量 | ≥ 8GB (可存储约 80,000 条资产) |

### 12.4 版本历史

| 版本 | 日期 | 变更内容 |
|------|------|---------|
| v1.0 | 2026-04-29 | 初始版本，定义全部上下行命令、帧格式、错误码、状态机 |
| **v2.0** | **2026-05-09** | **新增第13节 4G通道扩展(L610)，新增mqtt_connect/mqtt_publish/mqtt_disconnect/l610_status/l610_at命令及对应上行消息** |

---

## 13. 4G通道扩展(L610)

### 13.1 概述

L610模块通过UART2 (GPIO19 TX / GPIO20 RX) 与ESP32连接，作为4G MQTT代理。ESP32通过AT指令控制L610，WS63通过UART1下发mqtt命令给ESP32，ESP32转发给L610执行。

**与WiFi的关系：**
- **WiFi优先**：WS63首先判断WiFi是否可用
- WiFi可用 → WS63直连MQTT Broker上传
- WiFi不可用 → WS63通过UART1转发给ESP32 → L610 4G发布

**核心约束：**
- MQTTUSER凭据（用户名/密码/ClientID）由ESP32固件硬编码，WS63不需要下发
- L610的AT+MQTTPUB的Payload最大1024字节（ASCII模式）
- WS63发给ESP32的mqtt命令（mqtt_connect/mqtt_publish等）属于**查询类命令**，即使ESP32正在执行视觉任务也可立即处理

### 13.2 新增下行命令 (WS63 → ESP32)

#### 13.2.1 MQTT连接管理

```json
// 连接MQTT Broker (4G通道)
{
  "cmd": "mqtt_connect",
  "host": "demo.thingskit.com",
  "port": 1883,
  "clean_session": 1,
  "keepalive": 60
}

// 断开MQTT连接
{"cmd":"mqtt_disconnect"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | `"mqtt_connect"` 或 `"mqtt_disconnect"` |
| `host` | string | 是(mqtt_connect) | MQTT Broker地址 |
| `port` | uint16 | 是(mqtt_connect) | MQTT Broker端口 |
| `clean_session` | uint8 | 否 | 0/1，默认1 |
| `keepalive` | uint16 | 否 | 心跳间隔(秒)，默认60 |

#### 13.2.2 MQTT发布

```json
{
  "cmd": "mqtt_publish",
  "topic": "device/WS63-AA:BB:CC:DD:EE:FF/up",
  "payload": "{json payload}",
  "qos": 1,
  "retain": 0
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"mqtt_publish"` |
| `topic` | string | 是 | MQTT主题，255字节以内 |
| `payload` | string | 是 | JSON字符串，**1024字节以内** |
| `qos` | uint8 | 否 | QoS等级，默认1 |
| `retain` | uint8 | 否 | retain标志，默认0 |

#### 13.2.3 L610状态查询

```json
{"cmd":"l610_status"}
```

**上行响应：** `l610_status` (见§13.3.1)

#### 13.2.4 AT指令透传（调试用）

```json
{"cmd":"l610_at","at":"AT+CSQ"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"l610_at"` |
| `at` | string | 是 | 要透传的AT指令（不含`\r`） |

### 13.3 新增上行消息 (ESP32 → WS63)

#### 13.3.1 L610状态上报

```json
{
  "type": "l610_status",
  "mqtt_state": "connected",
  "signal_quality": 18
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"l610_status"` |
| `mqtt_state` | string | `"connected"` / `"disconnected"` / `"connecting"` / `"error"` |
| `signal_quality` | int | CSQ值(0-31)，99=未知 |

#### 13.3.2 MQTT发布结果

```json
{
  "type": "mqtt_publish_done",
  "result": "success",
  "topic": "device/WS63-AA:BB:CC:DD:EE:FF/up"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"mqtt_publish_done"` |
| `result` | string | `"success"` / `"failed"` |
| `topic` | string | 发布的Topic |

#### 13.3.3 MQTT连接状态通知

```json
{
  "type": "mqtt_connected",
  "state": "connected",
  "host": "demo.thingskit.com",
  "port": 1883
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"mqtt_connected"` |
| `state` | string | `"connected"` / `"disconnected"` |
| `host` | string | MQTT Broker地址 |
| `port` | uint16 | MQTT Broker端口 |

#### 13.3.4 L610错误通知

```json
{
  "type": "l610_error",
  "code": "L610_AT_TIMEOUT",
  "msg": "L610 not responding"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"l610_error"` |
| `code` | string | 错误码，见下表 |
| `msg` | string | 人类可读的错误描述

| code 取值 | 说明 |
|-----------|------|
| `L610_AT_TIMEOUT` | AT指令超时 |
| `L610_MQTT_CONNECT_FAIL` | MQTT连接失败 |
| `L610_MQTT_PUBLISH_FAIL` | MQTT发布失败 |
| `L610_MQTT_LOST_CONNECTION` | MQTT连接意外断开（收到+MQTTBREAK） |
| `L610_NOT_RESPONDING` | L610模块无响应（连续3次AT超时） |

### 13.4 时序流程：4G通道MQTT发布

```
WS63                                 ESP32-S3                    ADP-L610
 │                                      │                          │
 │  (WiFi不可用, 走4G)                   │                          │
 │                                      │                          │
 │  {"cmd":"mqtt_publish",              │                          │
 │   "topic":"device/WS63-AA:BB/up",    │                          │
 │   "payload":"{...}"}                 │                          │
 ├─────────────────────────────────────►│                          │
 │                                      │                          │
 │                                      │  AT+MQTTPUB=1,           │
 │                                      │  "device/WS63-.../up",   │
 │                                      │  1,0,"{...}"\r           │
 │                                      ├─────────────────────────►│
 │                                      │          OK              │
 │                                      │◄─────────────────────────┤
 │                                      │                          │
 │                                      │  +MQTTPUB: 1,1           │
 │                                      │◄─────────────────────────┤
 │                                      │                          │
 │  {"type":"mqtt_publish_done",         │                          │
 │   "result":"success",                │                          │
 │   "topic":"device/WS63-AA:BB/up"}    │                          │
 │◄─────────────────────────────────────┤                          │
```

### 13.5 L610 MQTT AT指令参考

| 指令 | 功能 |
|------|------|
| `AT+MQTTUSER` | 设置ClientID/用户名/密码 |
| `AT+MQTTOPEN` | 建立MQTT连接 |
| `AT+MQTTPUB` | 发布消息 |
| `AT+MQTTCLOSE` | 关闭MQTT连接 |
| `+MQTTBREAK` | URC意外断开通知（驱动需处理） |

完整语法详见 `docs/L610_4G_INTEGRATION_PLAN.md` 第4节。

### 13.6 云端统一JSON格式 (WiFi/4G共用)

无论走WiFi还是4G，发给ThingsKit的MQTT消息Payload格式完全一致：

```json
{
  "device_id": "WS63-AA:BB:CC:DD:EE:FF",
  "timestamp": 1712345678,
  "msg_type": "task_done",
  "task": "inventory",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success",
  "is_match": true,
  "weighted_confidence": 0.892,
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50,
  "upload_channel": "4g"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `device_id` | string | 是 | WS63设备标识 |
| `timestamp` | int64 | 是 | Unix时间戳（秒） |
| `msg_type` | string | 是 | 消息类型: `"task_done"` / `"error"` / `"status"` / `"pong"` |
| `upload_channel` | string | 是 | `"wifi"` / `"4g"` |
| ... | ... | ... | 其余字段与§7对应消息类型一致 |

### 13.7 新增错误码

| 错误码 | 说明 | 触发场景 |
|--------|------|---------|
| `L610_AT_TIMEOUT` | L610 AT指令超时 | L610模块未响应 |
| `L610_MQTT_CONNECT_FAIL` | MQTT连接失败 | L610无法连接MQTT Broker |
| `L610_MQTT_PUBLISH_FAIL` | MQTT发布失败 | L610发布消息异常 |
| `L610_MQTT_LOST_CONNECTION` | MQTT连接意外断开 | L610收到+MQTTBREAK URC |
| `L610_NOT_RESPONDING` | L610模块无响应 | 连续3次AT超时 |

### 13.8 数据流说明

```
ESP32完成AI推理
    ↓ task_done (UART1 → WS63)
WS63收到结果
    ↓ 组装云端统一JSON
    ↓ (判断WiFi是否可用)
    ├─ WiFi可用 → WS63自行MQTT上传 → 结束
    │
    └─ WiFi不可用 → {"cmd":"mqtt_publish",
    │                 "topic":"device/WS63-.../up",
    │                 "payload":"{...}"}
    │               ↓ UART1
    │           ESP32收到
    │               ↓ UART2 (AT+MQTTPUB)
    │           L610 → 4G → ThingsKit
    │               ↓ UART2 (+MQTTPUB: 1,1)
    │           ESP32回传结果
    │               ↓ UART1
    │           WS63收到 {"type":"mqtt_publish_done",...}
```
</content>
<task_progress>
- [x] 收集硬件信息（ADP板、J3引脚、电平、SIM卡）
- [x] 收集ThingsKit MQTT信息（broker/username/password）
- [x] 确认MQTT AT指令语法
- [x] 更新 docs/L610_4G_INTEGRATION_PLAN.md
- [x] 更新 docs/WS63_ESP32_PROTOCOL.md (追加v2.0第13节)
- [ ] 切换到ACT MODE开始写代码
</task_progress>


