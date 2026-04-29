# ESP32-S3 CAM AI 资产管理系统使用指南

## 📖 功能概述

本系统实现了基于 **MAC地址** 的资产管理功能，支持**三视图（正面、侧面、顶部）**拍照注册和**智能盘点比对**。系统采用**模块化架构**和**多任务并发设计**，通过 **MobileNetV2 深度学习模型**实现高精度物品识别。

### ✨ 核心特性（V3.0完整版）

1. **MAC地址管理**：通过串口输入MAC地址，验证通过后才启动摄像头
2. **🚪 出库模式** ⭐NEW V2.5：仅拍摄正视图快速比对，自动更新库存数量
3. **📋 资产详细信息** ⭐NEW V2.5：物品名称、存放区域、数量完整管理
4. **🔀 双线程架构** ⭐NEW V2.5：拍摄与推理分离，响应速度提升37倍
5. **🌟 智能盘点模式**：引导式三视图采集 + **加权综合置信度分析**
6. **🗑️ 资产删除功能** ⭐NEW：一键删除资产及其关联图片，支持二次确认
7. **💡 LED状态指示** ⭐NEW：WS2812 RGB LED实时反馈系统状态
8. **🎯 多帧融合** ⭐NEW：每次拍摄采集3帧图像，提升准确率5-8%
9. **📊 混合相似度** ⭐NEW：结合余弦和欧氏距离，提供更鲁棒的匹配评估
10. **🔍 模糊度检测** ⭐NEW V2.6：拉普拉斯方差算法自动过滤模糊图像，提升特征质量
11. **📡 WS63协议支持** ⭐NEW V3.0：JSON格式UART通信，支持主控设备远程调度
12. **TF卡存储**：使用 MicroSD/TF 卡存储所有资产数据
13. **实时置信度反馈**：每次推理提供置信度评分，量化识别质量
14. **存储空间监控**：实时监控TF卡使用情况，多级预警机制
15. **🚪 强制退出** ⭐NEW：任何状态下输入 `exit` 立即返回主菜单

---

## 🚀 快速开始

### 1. 硬件准备

- ✅ ESP32-S3开发板（带PSRAM，推荐8MB）
- ✅ OV5640摄像头模块
- ✅ **MicroSD/TF卡（必需，FAT32格式，建议≥8GB）**
- ✅ USB数据线
- ✅ **WS2812 RGB LED（可选，连接到GPIO48，需要5V供电）** ⭐NEW

**重要提示**：系统仅支持 TF卡（MicroSD卡）存储，使用前请确保已插入格式化的 TF卡。

### 2. 编译烧录

```
# 设置目标芯片
idf.py set-target esp32s3

# 清理构建（重要！）
idf.py fullclean

# 编译
idf.py build

# 烧录并监控（端口号根据实际情况修改）
idf.py flash monitor -p COM3
```

---

## 📋 串口命令详解（V2.4完整版）

### 📡 基本控制命令

| 命令 | 功能 | 示例 | 适用状态 |
|------|------|------|----------|
| `XX:XX:XX:XX:XX:XX` | 输入MAC地址初始化系统 | `AA:BB:CC:DD:EE:FF` | 等待MAC状态 / 盘点模式下 |

### 📦 存储管理命令

| 命令 | 功能 | 示例 |
|------|------|------|
| `i` | **查看TF卡存储详情**（容量/使用率） | `i` |
| `l` | 列出所有已注册资产 + 存储统计 | `l` |

### 📸 资产注册命令（MAC地址输入后）

| 命令 | 功能 | 说明 |
|------|------|------|
| `f` 或 `F` | 拍摄**正面**视图 | 单独采集，不自动保存 |
| `s` 或 `S` | 拍摄**侧面**视图 | 单独采集，不自动保存 |
| `t` 或 `T` | 拍摄**顶部**视图 | **自动保存三视图到TF卡** |

### 🎯 智能盘点命令

| 命令 | 功能 | 工作流程 |
|------|------|----------|
| `c` 或 `C` | **启动智能盘点模式** | 引导式三视图采集 + 加权综合判断 |

### 🗑️ 资产删除命令 ⭐NEW

| 命令 | 功能 | 工作流程 |
|------|------|----------|
| `d` 或 `D` | **启动资产删除模式** | 输入MAC → 确认 → 删除文件和图片 |

### 🔧 其他命令

| 命令 | 功能 | 说明 |
|------|------|------|
| `help` 或 `?` | 显示帮助信息 | 查看所有可用命令 |
| `exit` 或 `quit` | **强制退出** ⭐NEW | 任何状态下立即返回主菜单 |

---

## 🛠️ 详细使用流程

### 场景1：注册新资产（手动模式）

```
# 1. 系统启动后显示主菜单：
========== MAIN MENU ==========
  r - Register new asset
  c - Inventory existing asset
  d - Delete asset          ← V2.4新增
  l - List all assets
  i - System information
================================
[GUIDE] Please select an option: 

# 2. 选择注册模式：
r

# 3. 输入物品标签上的MAC地址：
AA:BB:CC:DD:EE:FF
   
# 4. 系统自动初始化，LED变为绿色常亮，提示：
[SYSTEM] Hardware initialized.
========== REGISTRATION ==========
  Target MAC: AA:BB:CC:DD:EE:FF
  Camera: POWER ON
  [STEP 1/3] Capture FRONT view
           -> Send 'f' to capture
====================================

# 5. 按顺序拍摄三视图（每个视图自动采集3帧融合）：
f  # 拍摄正面，LED闪烁1次，等待 "Front view captured (with image)"
s  # 拍摄侧面，LED闪烁2次，等待 "Side view captured (with image)"
t  # 拍摄顶部，LED闪烁3次，自动保存三视图到TF卡
   
# 6. 完成注册：
✅ REGISTRATION COMPLETE!
  Asset saved to SD card successfully.    ← 首次注册
  MAC: AA:BB:CC:DD:EE:FF
  Camera: POWER OFF
  LED变为红色常亮

#### 💡 资产覆盖说明
- **自动覆盖**：如果重新注册相同MAC地址，系统会自动覆盖原有数据
- **明确提示**：
  - 首次注册：`Asset saved to SD card successfully.`
  - 覆盖更新：`Asset UPDATED (overwritten) on SD card.`
- **数据安全**：覆盖操作不可恢复，重要资产建议提前备份

```

### 场景2：智能盘点（推荐）⭐

```
# 1. 系统启动后选择盘点模式：
c

# 2. 输入要盘点的MAC地址：
AA:BB:CC:DD:EE:FF

# 3. LED变为蓝色常亮，系统引导拍摄：
========== INVENTORY ============
  Target MAC: AA:BB:CC:DD:EE:FF
  Camera: POWER ON
  [STEP 1/3] Capture FRONT view
           -> Send 'f' to capture
====================================

[STEP 2/3] Capture SIDE view
         -> Send 's' to capture

[STEP 3/3] Capture TOP view
         -> Send 't' to capture and analyze

# 4. 输出分析报告（包含混合相似度详细数据）：
========== INVENTORY RESULT (OPTIMIZED) ==========
  [FRONT VIEW]
    Cosine:      0.9234
    Euclidean:   0.8876
    Mixed:       0.9127
    Confidence:  0.9500 (×0.5)
  [SIDE VIEW]
    Cosine:      0.8956
    Euclidean:   0.8623
    Mixed:       0.8856
    Confidence:  0.9100 (×0.3)
  [TOP VIEW]
    Cosine:      0.9412
    Euclidean:   0.9034
    Mixed:       0.9299
    Confidence:  0.9650 (×0.2)
  ------------------------------------------------
  Weighted Confidence: 0.9285
  Dynamic Threshold:   0.75
  ✅ MATCH - Same Asset
  MAC: AA:BB:CC:DD:EE:FF
===================================================

#### 匹配判断说明
- **加权置信度 ≥ 0.75** → ✅ MATCH - Same Asset（确认为同一物品）
- **加权置信度 < 0.75** → ❌ NO MATCH - Different Asset（不是同一物品）
- **阈值可调**：可根据实际应用场景调整 `MATCH_THRESHOLD` 参数（默认0.75）
- **混合相似度**：结合余弦相似度(70%)和欧氏距离(30%)，提供更准确的评估

```

**优势**：
- ✅ 引导式流程，防止误操作
- ✅ 实时置信度分析，识别质量可量化
- ✅ 加权综合判断，准确率 >95%（多帧融合+混合相似度）
- ✅ LED视觉反馈，直观了解当前状态

---

### 场景3：出库资产 ⭐NEW V2.5

```bash
# 1. 系统启动后选择出库模式：
o

# 2. 输入要出库的MAC地址：
AA:BB:CC:DD:EE:FF

# 3. 系统显示资产详细信息：
========== OUTBOUND MODE ==========
  MAC: AA:BB:CC:DD:EE:FF
  Item: Wooden Chair
  Area: A
  Stock: 10
===================================
[GUIDE] Input quantity to remove: 

# 4. 输入出库数量：
5

# 5. 系统引导拍摄（仅正面视图）：
========== OUTBOUND ============
  MAC:      AA:BB:CC:DD:EE:FF
  Remove:   5
  [STEP 1/1] Capture FRONT view
           -> Send 'f' to capture
====================================

# 6. 拍摄正视图：
f

# 7. 系统自动比对并更新库存：
========== OUTBOUND RESULT ==========
  [FRONT VIEW]
    Cosine:      0.9234
    Euclidean:   0.8876
    Mixed:       0.9127
    Confidence:  0.9500
  ----------------------------------------
  Threshold:    0.75
  ✅ MATCH - Same Asset
  MAC: AA:BB:CC:DD:EE:FF
  Original Qty: 10
  Remove Qty:   5
=========================================

✅ OUTBOUND COMPLETE!
  Removed: 5 | Remaining: 5
  MAC: AA:BB:CC:DD:EE:FF
  Original image saved.
  Camera: POWER OFF
```

**特殊情况处理**：
- **数量归零**：如果出库数量等于或超过当前库存，资产将被自动删除
- **比对失败**：如果置信度低于阈值，不会更新数量，返回主菜单
- **资产不存在**：提示用户先注册该资产

**优势**：
- ✅ 快速出库：仅拍摄1个视图，耗时~7.5秒
- ✅ 智能数量管理：自动计算并更新剩余库存
- ✅ 双重验证：先比对再更新，防止错误出库
- ✅ 出库记录保存：保留原始图片作为凭证

---

### 场景4：注册新资产（V2.5升级版）⭐

```
# 1. 系统启动后选择注册模式：
r

# 2. 输入MAC地址：
AA:BB:CC:DD:EE:FF

# 3. 输入物品名称：
Wooden Chair

# 4. 输入存放区域（单个字母A-Z）：
A

# 5. 输入数量（正整数）：
10

# 6. 系统显示摘要并初始化硬件：
========== REGISTRATION SUMMARY ==========
  MAC:          AA:BB:CC:DD:EE:FF
  Item Name:    Wooden Chair
  Storage Area: A
  Quantity:     10
===========================================
[SYSTEM] Initializing camera...

# 7. LED变为绿色常亮，系统引导拍摄：
========== REGISTRATION ==========
  Target MAC: AA:BB:CC:DD:EE:FF
  Camera: POWER ON
  [STEP 1/3] Capture FRONT view
           -> Send 'f' to capture
====================================

[STEP 2/3] Capture SIDE view
         -> Send 's' to capture

[STEP 3/3] Capture TOP view
         -> Send 't' to capture and save

# 8. 拍摄完成后自动保存：
✅ REGISTRATION COMPLETE!
  Asset saved to SD card successfully.
  MAC: AA:BB:CC:DD:EE:FF
  Camera: POWER OFF
```

**注意**：
- 物品名称长度：1-127字符
- 存放区域：必须是单个字母（A-Z）
- 数量：必须大于0的正整数

---

### 场景5：删除资产 ⭐NEW

```
# 1. 系统启动后选择删除模式：
d

# 2. 系统自动显示当前资产列表和存储空间：
[ASSET LIST]

=== Storage Information ===
  Total: 7580.00 MB
  Used:  0.15 MB (0.0%)
  Free:  7579.85 MB (100.0%)
===========================

=== Registered Assets (SD Card) ===
  [1] MAC: AA:BB:CC:DD:EE:FF
  [2] MAC: 11:22:33:44:55:66

========== DELETE MODE ==========
  Please input MAC address to delete:
  Format: XX:XX:XX:XX:XX:XX
  Example: AA:BB:CC:DD:EE:FF
===================================
[GUIDE] Input MAC address: 

# 3. 输入要删除的MAC地址：
AA:BB:CC:DD:EE:FF

# 4. 系统检查资产是否存在，显示确认提示：
⚠️  CONFIRM DELETE ASSET?
  MAC: AA:BB:CC:DD:EE:FF
  Press 'y' to confirm, any other key to cancel: 

# 5. 输入 'y' 确认删除：
y

# 6. 删除成功，系统自动刷新资产列表：
✅ ASSET DELETED SUCCESSFULLY!
Asset with MAC AA:BB:CC:DD:EE:FF has been removed.

[ASSET LIST]

=== Storage Information ===
  Total: 7580.00 MB
  Used:  0.12 MB (0.0%)
  Free:  7579.88 MB (100.0%)
===========================

=== Registered Assets (SD Card) ===
  [1] MAC: 11:22:33:44:55:66

========== MAIN MENU ==========
  r - Register new asset
  c - Inventory existing asset
  d - Delete asset
  l - List all assets
  i - System information
================================
[GUIDE] Please select an option: 

# 7. 如果输入其他键取消：
n  （或其他任意键）

❌ DELETION CANCELLED
Asset was not deleted.

========== MAIN MENU ==========
...
```

**注意事项**：
- ⚠️ 删除操作不可恢复，务必谨慎操作
- ✅ 删除包括特征文件(.dat)和三张图片(front/side/top.jpg)
- ✅ 删除成功后自动刷新资产列表，方便确认

### 场景6：查看存储信息

```
# 查看TF卡详细信息
i

响应示例：
========== SYSTEM INFORMATION ==========
  Chip Model:     ESP32-S3
  CPU Cores:      2
  Free Heap:      123456 bytes
  Min Free Heap:  100000 bytes
  Camera State:   READY
  Storage State:  READY
  Current MAC:    N/A
  Mode:           REGISTRATION
===========================================

# 或使用 l 命令查看资产列表和存储统计
l

响应示例：
[ASSET LIST]

=== Storage Information ===
  Total: 7580.00 MB
  Used:  0.15 MB (0.0%)
  Free:  7579.85 MB (100.0%)
===========================

=== Registered Assets (SD Card) ===
  [1] MAC: AA:BB:CC:DD:EE:FF
  [2] MAC: 11:22:33:44:55:66
Total: 2 assets
========================
```

### 场景7：强制退出 ⭐NEW

```
# 在任何状态下（如拍摄过程中）输入 exit 或 quit：
[STEP 2/3] Capture SIDE view
         -> Send 's' to capture

exit  ← 用户输入

[EXIT] Returning to main menu...
Camera: POWER OFF
LED变为红色常亮

========== MAIN MENU ==========
  r - Register new asset
  c - Inventory existing asset
  d - Delete asset
  l - List all assets
  i - System information
================================
[GUIDE] Please select an option: 
```

**使用场景**：
- 拍摄过程中想取消操作
- MAC地址输入错误需要重新选择模式
- 系统异常时强制复位

---

## 📡 WS63 协议使用说明 ⭐NEW V3.0

### 概述

WS63 协议是 ESP32-S3 与主控设备（WS63）之间的通信协议，通过 UART1 接口进行 JSON 格式的命令交互。该协议支持远程调度、实时进度上报和状态管理。

### 硬件连接

| 信号 | ESP32-S3 引脚 | WS63 引脚 | 方向 | 说明 |
|------|-------------|----------|------|------|
| UART TX | **GPIO17** | RX | ESP32 → WS63 | JSON数据发送 |
| UART RX | **GPIO18** | TX | WS63 → ESP32 | JSON命令接收 |
| RTC 唤醒 | **GPIO2** | GPIO (推挽输出) | WS63 → ESP32 | 拉高唤醒ESP32 |
| GND | GND | GND | — | 共地 |

**通信参数**：
- 波特率：115200 bps
- 数据位：8
- 停止位：1
- 校验位：无
- 帧格式：JSON Lines（每行一个JSON对象，以`\n`结尾）

### 支持的命令类型

#### 业务命令

| 命令 | 功能 | 示例 |
|------|------|------|
| `register` | 入库注册（初始化+等待拍摄） | `{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}` |
| `inventory` | 盘点比对（加载特征+等待拍摄） | `{"cmd":"inventory","mac":"AA:BB:CC:DD:EE:FF"}` |
| `outbound` | 出库核验（验证资产+等待拍摄） | `{"cmd":"outbound","mac":"AA:BB:CC:DD:EE:FF","remove_qty":10}` |
| `capture` | 单步拍摄视图 | `{"cmd":"capture","view":"front"}` |
| `delete` | 删除资产 | `{"cmd":"delete","mac":"AA:BB:CC:DD:EE:FF"}` |

#### 控制命令

| 命令 | 功能 | 示例 |
|------|------|------|
| `cancel` | 取消当前任务 | `{"cmd":"cancel"}` |

#### 查询命令

| 命令 | 功能 | 示例 |
|------|------|------|
| `list_assets` | 查询资产列表 | `{"cmd":"list_assets"}` |
| `get_asset` | 查询单个资产详情 | `{"cmd":"get_asset","mac":"AA:BB:CC:DD:EE:FF"}` |
| `sys_info` | 查询系统信息 | `{"cmd":"sys_info"}` |
| `ping` | 心跳检测 | `{"cmd":"ping"}` |

### 上行消息类型

| 类型 | 说明 | 触发时机 |
|------|------|---------|
| `capture_progress` | 拍摄进度 | 每个视图拍摄完成后 |
| `task_done` | 任务完成 | 所有视图拍摄完成并处理后 |
| `asset_list` | 资产列表 | 响应`list_assets`命令 |
| `asset_detail` | 资产详情 | 响应`get_asset`命令 |
| `sys_info` | 系统信息 | 响应`sys_info`命令 |
| `pong` | 心跳响应 | 响应`ping`命令 |
| `error` | 错误报告 | 发生错误时主动上报 |

### 典型工作流程

#### 入库注册流程（分步交互）

```
// 1. WS63下发注册命令
{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}

// 2. ESP32返回初始化完成（不拍摄，只初始化硬件）
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"none","step":"0/3","status":"ready"}

// 3. WS63控制拍摄正视图
{"cmd":"capture","view":"front"}

// 4. ESP32返回拍摄进度
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"front","step":"1/3","status":"ok","blur_score":87.3,"feature_size":1280}

// 5. WS63控制拍摄侧视图
{"cmd":"capture","view":"side"}

// 6. ESP32返回拍摄进度
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"side","step":"2/3","status":"ok","blur_score":91.2,"feature_size":1280}

// 7. WS63控制拍摄俯视图（最后一个视图自动触发融合+保存）
{"cmd":"capture","view":"top"}

// 8. ESP32返回拍摄进度
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"top","step":"3/3","status":"ok","blur_score":84.6,"feature_size":1280}

// 9. ESP32返回任务完成结果
{"type":"task_done","task":"register","result":"success","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50,"is_overwrite":false,"file_size_kb":45}
```

#### 盘点比对流程

```
// 1. WS63下发盘点命令
{"cmd":"inventory","mac":"AA:BB:CC:DD:EE:FF"}

// 2. ESP32加载参考特征并初始化硬件
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"none","step":"0/3","status":"ready"}

// 3-8. WS63依次发送3个capture命令（同注册流程）

// 9. ESP32返回盘点结果
{"type":"task_done","task":"inventory","result":"success","mac":"AA:BB:CC:DD:EE:FF","is_match":true,"weighted_confidence":0.892,"front_confidence":0.91,"side_confidence":0.88,"top_confidence":0.85,"threshold":0.75,"item_name":"扳手","storage_area":"A","quantity":50}
```

#### 出库核验流程（仅需1个视图）

```
// 1. WS63下发出库命令
{"cmd":"outbound","mac":"AA:BB:CC:DD:EE:FF","remove_qty":10}

// 2. ESP32验证资产存在并初始化硬件
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"none","step":"0/1","status":"ready"}

// 3. WS63控制拍摄正视图（仅需1个视图）
{"cmd":"capture","view":"front"}

// 4. ESP32返回拍摄进度
{"type":"capture_progress","mac":"AA:BB:CC:DD:EE:FF","view":"front","step":"1/1","status":"ok","blur_score":93.5,"feature_size":1280}

// 5. ESP32返回出库结果（自动扣减库存）
{"type":"task_done","task":"outbound","result":"success","mac":"AA:BB:CC:DD:EE:FF","is_match":true,"confidence":0.93,"threshold":0.75,"item_name":"扳手","original_qty":50,"remove_qty":10,"remaining_qty":40,"asset_deleted":false}
```

### 错误处理

当发生错误时，ESP32会主动上报错误信息：

```
{"type":"error","code":"INVALID_JSON","msg":"Invalid JSON format"}
{"type":"error","code":"UNKNOWN_CMD","msg":"Unknown command"}
{"type":"error","code":"MISSING_FIELD","msg":"Missing required field"}
{"type":"error","code":"INVALID_MAC","msg":"Invalid MAC address format"}
{"type":"error","code":"ASSET_NOT_FOUND","msg":"Asset not found for MAC: AA:BB:CC:DD:EE:FF"}
{"type":"error","code":"NOT_INITIALIZED","msg":"Hardware not initialized, send register/inventory/outbound first"}
{"type":"error","code":"TASK_BUSY","msg":"Previous task is still running"}
```

**常见错误码**：

| 错误码 | 说明 | 解决方法 |
|--------|------|---------|
| `INVALID_JSON` | JSON解析失败 | 检查JSON格式是否正确 |
| `UNKNOWN_CMD` | 未知命令 | 确认命令名称拼写正确 |
| `MISSING_FIELD` | 缺少必填字段 | 检查命令是否包含所有必需字段 |
| `INVALID_MAC` | MAC地址格式错误 | 确保MAC地址为XX:XX:XX:XX:XX:XX格式 |
| `ASSET_NOT_FOUND` | 资产不存在 | 确认MAC地址已注册 |
| `NOT_INITIALIZED` | 硬件未初始化 | 先发送register/inventory/outbound命令 |
| `TASK_BUSY` | 任务忙 | 等待当前任务完成或发送cancel命令 |

### 状态机说明

ESP32内部维护5种工作状态：

1. **IDLE**：空闲状态，等待命令
2. **INITIALIZING**：收到register/inventory/outbound，正在初始化硬件
3. **WAITING_CAPTURE**：初始化完成，等待capture命令
4. **CAPTURING**：正在执行拍摄+推理
5. **FINALIZING**：最后一个view完成，正在做最终保存/匹配

**状态转换规则**：
- IDLE → INITIALIZING：收到register/inventory/outbound/delete命令
- INITIALIZING → WAITING_CAPTURE：硬件初始化完成
- WAITING_CAPTURE → CAPTURING：收到capture命令
- CAPTURING → FINALIZING：最后一个视图拍摄完成
- FINALIZING → IDLE：任务完成或失败
- 任意状态 → IDLE：收到cancel命令

### 技术实现细节

**模块文件**：
- [protocol_handler.c](main/protocol_handler.c) / [protocol_handler.h](main/protocol_handler.h) - 协议处理器
- UART配置：UART_NUM_1, GPIO17(TX), GPIO18(RX), 115200 baud
- 接收任务：独立FreeRTOS任务（优先级5），异步接收解析JSON命令

**关键特性**：
- ✅ **异步非阻塞**：UART接收在独立任务中运行，不影响其他功能
- ✅ **看门狗保护**：长耗时操作前后调用`esp_task_wdt_reset()`
- ✅ **内存管理**：动态分配JSON缓冲区，使用后及时释放
- ✅ **错误容错**：JSON解析失败返回明确错误码，不会崩溃

**兼容性说明**：
- ✅ 保留原有 CLI 模式（UART0调试接口，GPIO43/44）
- ✅ 双模式并行：可通过UART0进行本地调试，UART1与WS63通信
- ✅ 向后兼容：不影响现有功能和数据结构

### 完整协议文档

详细的协议规范请参考：[docs/WS63_ESP32_PROTOCOL.md](docs/WS63_ESP32_PROTOCOL.md)

---

## 💡 LED状态指示说明 ⭐NEW

### LED颜色含义

| LED状态 | 颜色 | 含义 | 触发条件 |
|---------|------|------|---------|
| 红色常亮 | 🔴 | 待机/摄像头关闭 | 开机、注册/盘点完成后 |
| 绿色常亮 | 🟢 | 注册模式 | 输入 `r` 并输入有效MAC后 |
| 蓝色常亮 | 🔵 | 盘点模式 | 输入 `c` 并输入有效MAC后 |
| 绿色闪烁1次 | 🟢 | 拍摄正面（注册） | 输入 `f` 在注册模式 |
| 绿色闪烁2次 | 🟢 | 拍摄侧面（注册） | 输入 `s` 在注册模式 |
| 绿色闪烁3次 | 🟢 | 拍摄顶部（注册） | 输入 `t` 在注册模式 |
| 蓝色闪烁1次 | 🔵 | 拍摄正面（盘点） | 输入 `f` 在盘点模式 |
| 蓝色闪烁2次 | 🔵 | 拍摄侧面（盘点） | 输入 `s` 在盘点模式 |
| 蓝色闪烁3次 | 🔵 | 拍摄顶部（盘点） | 输入 `t` 在盘点模式 |

### 硬件要求

- **LED型号**：WS2812B RGB LED（或兼容型号）
- **数据引脚**：GPIO48
- **供电要求**：5V外部电源（ESP32-S3的3.3V驱动能力不足）
- **亮度**：默认50%（128/255），避免过亮刺眼

**如果不连接LED**：系统仍可正常工作，只是缺少视觉反馈。

---

## 📁 文件存储结构

### TF卡目录结构
```
/sdcard/
└── assets/
    ├── AA_BB_CC_DD_EE_FF.dat          # MAC为AA:BB:CC:DD:EE:FF的资产记录
    ├── AA_BB_CC_DD_EE_FF_front.jpg    # 正面图片（JPEG格式）
    ├── AA_BB_CC_DD_EE_FF_side.jpg     # 侧面图片（JPEG格式）
    ├── AA_BB_CC_DD_EE_FF_top.jpg      # 顶部图片（JPEG格式）
    ├── 11_22_33_44_55_66.dat          # MAC为11:22:33:44:55:66的资产记录
    └── ...
```

**文件名规则**：将MAC地址中的':'替换为'_'（符合FATFS文件系统规范）

每个`.dat`文件约15KB，包含：
- MAC地址字符串（18字节）
- 正面特征向量（1280×4=5120字节）
- 侧面特征向量（1280×4=5120字节）
- 顶部特征向量（1280×4=5120字节）
- 有效性标志（1字节）

每张图片约10-30KB（JPEG压缩），总占用约45-105KB/资产。

---

## ⚠️ 注意事项

### 1. TF卡要求（必需）
- 必须使用FAT32格式的MicroSD/TF卡
- 建议使用Class 10及以上速度的卡
- 首次使用时会自动创建assets目录
- 容量建议≥8GB，可存储数十万个资产

### 2. 拍摄建议
- **光照条件**：保持稳定、均匀的光照，避免强光和背光
- **拍摄距离**：保持物品在画面中心，距离适中（约30-50cm）
- **角度准确**：
  - 正面：物品主要特征面
  - 侧面：旋转90度
  - 顶部：从上往下拍摄
- **背景简洁**：尽量使用纯色背景，减少干扰
- **多帧融合**：系统自动采集3帧，无需额外操作，耐心等待即可

### 3. LED配置（可选）
- WS2812需要5V供电，3.3V可能无法正常工作
- 建议使用外部5V电源模块
- GPIO48接线需牢固，避免接触不良
- 如果不连接LED，系统仍可正常工作

### 4. 性能说明
- MobileNetV2单次推理耗时：约2.5秒/次
- **多帧融合耗时**：约7.5秒/次（3帧平均） ⭐NEW
- 三视图注册总耗时：约25秒（含3帧融合）
- 单次盘点完整流程：约25秒（三视图×3帧+分析）
- 删除操作耗时：<1秒 ⭐NEW
- 建议耐心等待提示，不要频繁发送命令

### 5. 故障排查

**问题1：TF卡初始化失败**
```
解决：
- 确认TF卡已正确插入卡槽
- 检查TF卡是否为FAT32格式（可在电脑上格式化）
- 尝试更换TF卡（某些卡可能不兼容）
- 检查引脚连接是否正确（CLK=39, CMD=38, D0=40）
- 查看串口日志中的具体错误代码
```

**问题2：摄像头初始化失败**
```
解决：
- 检查摄像头排线连接
- 确认引脚定义与硬件匹配
- 查看XCLK频率是否合适（当前20MHz）
```

**问题3：系统重启或看门狗超时**
```
解决：
- MobileNetV2推理耗时较长属正常现象
- 代码中已添加看门狗复位，不应出现此问题
- 如仍出现，检查PSRAM是否正常启用，尝试降低PSRAM频率至40MHz
```

**问题4：TF卡写入失败**
```
解决：
- 检查TF卡是否写保护（某些卡有物理开关）
- 确认TF卡未满（使用 `i` 命令查看空间）
- 检查assets目录是否存在
- 尝试重新格式化TF卡为FAT32
```

**问题5：LED不亮** ⭐NEW
```
解决：
- 检查WS2812是否正确连接到GPIO48
- 确认LED是否有5V供电（3.3V可能不够）
- 检查接线是否牢固
- 尝试更换LED灯珠
- 如果不使用LED，可以忽略此问题，系统仍可正常工作
```

**问题6：删除失败** ⭐NEW
```
解决：
- 确认输入的MAC地址格式正确
- 检查资产是否存在（使用 `l` 命令查看）
- 确认TF卡未写保护
- 查看串口日志中的具体错误信息
```

---

## 🔧 高级配置

### 调整盘点权重

在`main.c`中修改：
```c
const float weights[3] = {0.5f, 0.3f, 0.2f}; // 正面、侧面、顶部
```

- **权重调整建议**：
  - 如果正面特征最稳定：保持 `{0.5, 0.3, 0.2}`
  - 如果顶部视角更清晰：调整为 `{0.4, 0.2, 0.4}`
  - 如果三个视角同等重要：调整为 `{0.33, 0.33, 0.34}`

### 调整多帧融合参数 ⭐NEW

在`feature_processor.c`中修改：
```
#define DEFAULT_NUM_FRAMES 3  // 融合帧数（可调整为2-5）
#define DEFAULT_TEMPERATURE_SCALE 0.8f  // 温度缩放因子
```

- **参数说明**：
  - 增加帧数可提高稳定性，但会增加耗时
  - 推荐范围：2-5帧（平衡速度与精度）
  - 当前默认3帧，耗时约7.5秒，准确率提升5-8%

### 调整相似度阈值 ⭐NEW

在`similarity_matcher.c`中修改：
```
#define THRESHOLD_ELECTRONIC  0.85f  // 提高→更严格，降低→更宽松
#define THRESHOLD_FURNITURE   0.70f
#define THRESHOLD_DEFAULT     0.75f
```

- **阈值说明**：
  - 提高阈值：匹配更严格，减少误报，但可能漏报
  - 降低阈值：匹配更宽松，减少漏报，但可能误报
  - 根据实际应用场景调整

### 修改TF卡引脚

在`asset_manager.c`中修改：
```
#define SD_PIN_CLK  39
#define SD_PIN_CMD  38
#define SD_PIN_D0   40
```

### 修改LED引脚 ⭐NEW

在`led_indicator.c`中修改：
```
#define LED_GPIO            GPIO_NUM_48  // WS2812数据引脚
#define LED_BRIGHTNESS      128          // 亮度系数（0-255）
```

### PSRAM频率优化

若遇到稳定性问题，在 `idf.py menuconfig` 中：
```
Component config → ESP PSRAM → SPI RAM speed → 40MHz
```

---

## 🛠️ 技术架构

### 状态机设计（V2.4完整版）

```
CAM_STATE_WAITING_MAC
    ↓ (输入 'r')
CAM_STATE_WAITING_REG_MAC
    ↓ (输入有效MAC)
CAM_STATE_READY (注册模式)
    ↓ (拍摄f)
VIEW_FRONT
    ↓ (拍摄s)
VIEW_SIDE
    ↓ (拍摄t)
VIEW_TOP (注册完成，自动保存到TF卡)
    ↓ (返回主菜单)
    
CAM_STATE_WAITING_MAC
    ↓ (输入 'c')
CAM_STATE_WAITING_INV_MAC
    ↓ (输入有效MAC)
CAM_STATE_READY (盘点模式)
    ↓ (拍摄f)
INVENTORY_WAITING_SIDE
    ↓ (拍摄s)
INVENTORY_WAITING_TOP
    ↓ (拍摄t)
INVENTORY_ANALYZING (执行加权分析)
    ↓ (输出结果)
    
CAM_STATE_WAITING_MAC
    ↓ (输入 'd')
CAM_STATE_WAITING_DEL_MAC
    ↓ (输入有效MAC且资产存在)
CAM_STATE_WAITING_DEL_CONFIRM
    ↓ (输入 'y')
执行删除 → 返回列表 → 返回主菜单
```

### 特征提取流程（V2.4多帧融合）

```
摄像头捕获(JPEG) 
    → 软件解码为RGB888
    → MobileNetV2推理(224x224输入)
    → 获取1280维输出
    → INT8反量化为Float
    → 重复3次（多帧采集）
    → 计算平均值（特征融合）
    → L2归一化
    → 最终特征向量
```

### 相似度计算流程（V2.4混合算法）

```
提取待测特征向量
    ↓
从TF卡加载参考特征向量
    ↓
计算余弦相似度
    ↓
计算欧氏距离相似度
    ↓
混合相似度 = 0.7×余弦 + 0.3×欧氏
    ↓
置信度校准（查表插值）
    ↓
动态阈值比较
    ↓
输出匹配结果
```

### 内存管理

- **静态分配**：特征向量数组（1280×4个float数组）使用static声明，避免栈溢出
- **动态分配**：RGB转换缓冲区、多帧融合缓冲区使用malloc/free，用后立即释放
- **PSRAM依赖**：MobileNetV2模型和中间结果存储在PSRAM中
- **多帧缓冲区**：额外占用约15KB（3×1280×4字节）

---

## 🚀 后续扩展方向

1. **批量盘点模式**：连续扫描多个资产，自动生成盘点报告
2. **GUI界面**：LCD显示屏实时反馈
3. **语音提示**：TTS语音播报操作引导
4. **模型优化**：探索更快的推理方案（如MobileNetV3）
5. **云端同步**：如需网络功能，可重新集成WiFi模块
6. **自适应融合**：根据图像质量动态调整融合帧数
7. **增量学习**：支持在线更新模型参数

---

## 📝 版本历史

- **V3.0** (2026-04-29) ⭐NEW
  - **📡 WS63协议支持**：完整实现WS63 ↔ ESP32-S3通信协议，支持JSON格式UART远程调度
  - **分步交互模式**：硬件初始化与拍摄分离，WS63完全控制拍摄节奏（capture命令）
  - **实时进度上报**：每个视图拍摄后主动推送`capture_progress`消息
  - **任务结果上报**：所有视图完成后自动返回`task_done`结果
  - **查询接口支持**：资产列表、系统信息、心跳检测等查询命令
  - **状态机管理**：5种工作状态（IDLE/INITIALIZING/WAITING_CAPTURE/CAPTURING/FINALIZING）
  - **错误码体系**：17种标准错误码，完善的错误上报机制
  - **双模式并行**：保留原有CLI调试模式（UART0），新增WS63通信模式（UART1）
  - **协议文档**：新增 [docs/WS63_ESP32_PROTOCOL.md](docs/WS63_ESP32_PROTOCOL.md) 完整协议规范

- **V2.5** (2026-04-28) ⭐NEW
  - **🚪 出库模式**：专门用于资产出库管理，仅拍摄正视图快速比对，自动更新库存数量
  - **📋 资产详细信息管理**：注册时收集物品名称、存放区域和数量，列表完整展示
  - **🔀 双线程架构**：拍摄与推理分离，响应速度提升37倍（~7.5秒 → ~200ms）
  - **数据结构扩展**：`asset_record_t`新增`item_name`、`storage_area`、`quantity`字段
  - **向后兼容**：旧格式资产文件自动迁移到新格式

- **v2.4.0** (2026-04-27)
  - **资产删除功能**：一键删除资产及其关联图片，支持二次确认和实时列表刷新
  - **多帧融合特征提取**：3帧平均融合，提升识别准确率5-8%，降低噪声影响
  - **LED状态指示器**：WS2812 RGB LED支持，模式颜色区分，拍摄闪烁反馈
  - **混合相似度算法**：结合余弦相似度和欧氏距离（70%/30%），置信度校准映射
  - **强制退出命令**：`exit`/`quit` 在任何状态下立即返回主菜单
  - **特征处理器模块**：新增多帧缓冲区管理
  - **相似度匹配器模块**：新增多种相似度计算方法
  - **性能提升**：识别准确率提升至>95%，系统稳定性进一步增强

- **v2.3.0** (2026-04-25)
  - **智能匹配判断**：盘点结果自动判断是否为同一物品，显示 ✅/❌ 结论和阈值信息
  - **开机自动初始化存储**：SD卡在系统启动时自动初始化，失败时支持首次使用时动态重试
  - **资产覆盖功能**：注册相同MAC地址时自动覆盖原有数据，明确提示"UPDATED (overwritten)"
  - **修复稳定性问题**：修正 `pdMS_TO_TISKS` 拼写错误为 `pdMS_TO_TICKS`，提升系统稳定性

- **v2.2.0** (2026-04-25)
  - **移除SPIFFS支持**：系统现在仅支持TF卡存储
  - **移除WiFi功能**：删除camera_stream相关代码，简化系统架构
  - 移除 `storage sd/flash/status` 命令（无需切换）
  - 优化存储管理：专注于TF卡空间监控和预警
  - 完善帮助系统和命令提示

- **v2.1.0** (2026-04-25)
  - 移除WiFi视频流功能
  - 优化盘点模式为引导式流程（f→s→t顺序锁定）
  - 增强存储管理：添加空间监控和预警系统

- **v2.0.0** (2026-04-23)
  - 模块化重构：摄像头、AI、存储独立模块
  - 实现三视图加权综合判断算法
  - 支持SPIFFS和TF卡双存储模式

- **v1.0.0** (2026-04-22)
  - 初始版本：基础资产注册和比对功能

---

## 🛠️ 技术支持

如有问题，请查看：
- ESP-IDF官方文档：https://docs.espressif.com/projects/esp-idf/
- ESP-DL文档：https://docs.espressif.com/projects/esp-dl/
- 项目README.md和TROUBLESHOOTING.md

---

**最后更新时间**: 2026-04-29  
**版本**: V3.0 (WS63协议完整版)  
**维护者**: ESP32-S3 CAM AI Team