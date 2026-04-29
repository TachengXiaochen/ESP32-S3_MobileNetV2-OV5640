# ESP32-S3 CAM AI 快速参考卡 V2.6

## 📌 常用命令速查

### 🔑 模式选择（开机后首先选择）
```bash
r                    # 选择注册模式（入库）
o                    # 选择出库模式 ⭐NEW V2.5
c                    # 选择盘点模式
d                    # 选择删除模式
```

### 🔑 MAC地址管理
```bash
AA:BB:CC:DD:EE:FF    # 输入MAC地址（选择模式后输入）
```

**注意**：系统在启动时自动初始化TF卡，无需手动切换存储模式。

---

## 📡 WS63 协议快速参考 ⭐NEW V3.0

### 硬件连接
- **UART TX**: GPIO17 → WS63 RX
- **UART RX**: GPIO18 ← WS63 TX
- **RTC唤醒**: GPIO2 ← WS63 GPIO
- **波特率**: 115200 bps
- **帧格式**: JSON Lines（每行一个JSON对象，以`\n`结尾）

### 核心命令速查

#### 业务命令（下行 WS63 → ESP32）
```json
{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}
{"cmd":"inventory","mac":"AA:BB:CC:DD:EE:FF"}
{"cmd":"outbound","mac":"AA:BB:CC:DD:EE:FF","remove_qty":10}
{"cmd":"capture","view":"front"}  // front/side/top
{"cmd":"delete","mac":"AA:BB:CC:DD:EE:FF"}
```

#### 控制命令
```json
{"cmd":"cancel"}  // 取消当前任务
```

#### 查询命令
```json
{"cmd":"list_assets"}
{"cmd":"get_asset","mac":"AA:BB:CC:DD:EE:FF"}
{"cmd":"sys_info"}
{"cmd":"ping"}
```

### 上行消息类型（ESP32 → WS63）

| 类型 | 说明 | 示例 |
|------|------|------|
| `capture_progress` | 拍摄进度 | `{"type":"capture_progress","view":"front","step":"1/3","status":"ok","blur_score":87.3}` |
| `task_done` | 任务完成 | `{"type":"task_done","task":"register","result":"success",...}` |
| `asset_list` | 资产列表 | `{"type":"asset_list","count":3,"assets":[...]}` |
| `error` | 错误报告 | `{"type":"error","code":"INVALID_MAC","msg":"..."}` |

### 典型工作流程

#### 入库注册（分步交互）
```
WS63发送: {"cmd":"register","mac":"AA:BB:CC:DD:EE:FF","item_name":"扳手","storage_area":"A","quantity":50}
ESP32回复: {"type":"capture_progress","view":"none","step":"0/3","status":"ready"}

WS63发送: {"cmd":"capture","view":"front"}
ESP32回复: {"type":"capture_progress","view":"front","step":"1/3","status":"ok","blur_score":87.3}

WS63发送: {"cmd":"capture","view":"side"}
ESP32回复: {"type":"capture_progress","view":"side","step":"2/3","status":"ok","blur_score":91.2}

WS63发送: {"cmd":"capture","view":"top"}  // 最后一个视图自动触发融合+保存
ESP32回复: {"type":"capture_progress","view":"top","step":"3/3","status":"ok","blur_score":84.6}
ESP32回复: {"type":"task_done","task":"register","result":"success","is_overwrite":false,"file_size_kb":45}
```

### 常见错误码

| 错误码 | 说明 | 解决方法 |
|--------|------|---------|
| `INVALID_JSON` | JSON解析失败 | 检查JSON格式 |
| `UNKNOWN_CMD` | 未知命令 | 确认命令拼写 |
| `MISSING_FIELD` | 缺少必填字段 | 补全必需字段 |
| `INVALID_MAC` | MAC格式错误 | 使用XX:XX:XX:XX:XX:XX格式 |
| `ASSET_NOT_FOUND` | 资产不存在 | 确认MAC已注册 |
| `NOT_INITIALIZED` | 硬件未初始化 | 先发register/inventory/outbound |
| `TASK_BUSY` | 任务忙 | 等待或发送cancel |

### 技术要点
- ✅ **异步非阻塞**：UART接收在独立FreeRTOS任务中运行
- ✅ **看门狗保护**：长耗时操作调用`esp_task_wdt_reset()`
- ✅ **双模式并行**：UART0调试 + UART1 WS63通信
- ✅ **状态机管理**：5种状态确保流程可控

**完整协议文档**：[docs/WS63_ESP32_PROTOCOL.md](docs/WS63_ESP32_PROTOCOL.md)

---

### 📷 资产注册（四步输入 + 三视图拍摄）⭐V2.5升级
```bash
# 第一步：选择注册模式
r

# 第二步：输入MAC地址
AA:BB:CC:DD:EE:FF

# 第三步：输入物品名称
Wooden Chair

# 第四步：输入存放区域（单个字母A-Z）
A

# 第五步：输入数量（正整数）
10

# 第六步：按顺序拍摄三视图
f                    # 拍摄正面视图 (Front)
s                    # 拍摄侧面视图 (Side)
t                    # 拍摄顶部视图并保存 (Top)
```
**顺序要求：** f → s → t（必须按此顺序）

**覆盖提示**：
- 首次注册：`Asset saved to SD card successfully.`
- 覆盖更新：`Asset UPDATED (overwritten) on SD card.`

**智能检测**：系统会自动检测图像模糊度，模糊图像将被自动丢弃并重拍 ⭐NEW V2.6

### 🚪 出库模式 ⭐NEW V2.5
```bash
# 第一步：选择出库模式
o

# 第二步：输入MAC地址
AA:BB:CC:DD:EE:FF

# 第三步：系统显示资产信息
========== OUTBOUND MODE ==========
  MAC: AA:BB:CC:DD:EE:FF
  Item: Wooden Chair
  Area: A
  Stock: 10
===================================

# 第四步：输入出库数量
5

# 第五步：拍摄正视图（仅1个视图）
f

✅ 完成！库存已自动更新
  Removed: 5 | Remaining: 5
```

### 🔍 智能盘点（引导式）
```bash
c                    # 进入盘点模式（自动引导拍摄三视图）
```

**盘点结果示例**：
```
========== INVENTORY RESULT ==========
  Front: 92.56 (×0.5)
  Side:  89.29 (×0.3)
  Top:   95.12 (×0.2)
  ----------------------------------------
  Weighted Confidence: 91.8745
  Threshold: 0.75
  ✅ MATCH - Same Asset
  MAC: AA:BB:CC:DD:EE:FF
========================================
```

**匹配判断**：
- ✅ **置信度 ≥ 0.75** → 确认为同一物品
- ❌ **置信度 < 0.75** → 不是同一物品

### 🗑️ 删除资产 ⭐NEW
```bash
d                    # 进入删除模式
AA:BB:CC:DD:EE:FF    # 输入要删除的MAC地址
y                    # 确认删除
```

### 📋 其他功能
```bash
l                    # 列出所有已注册资产 + TF卡空间统计（显示完整信息）
i                    # 查看系统信息（堆内存、SDK版本等）
help / ?             # 显示帮助信息
exit / quit          # 强制退出到主菜单（任何状态可用）
```

---

## 🔄 典型工作流程

### 场景1：注册新资产（V2.5升级版）
```bash
1. r                     ← 选择注册模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入MAC地址

3. Wooden Chair          ← 输入物品名称

4. A                     ← 输入存放区域

5. 10                    ← 输入数量
   （系统自动初始化TF卡和摄像头）

6. f                     ← 拍摄正面
   （将物品正面对准摄像头）

7. s                     ← 拍摄侧面
   （将物品旋转90度）

8. t                     ← 拍摄顶部并保存
   
✅ 完成！资产已保存到TF卡
   首次注册：Asset saved to SD card successfully.
   覆盖更新：Asset UPDATED (overwritten) on SD card.
```

### 场景2：出库资产 ⭐NEW V2.5
```bash
1. o                     ← 选择出库模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入要出库的MAC地址
   （系统显示资产详细信息）

3. 5                     ← 输入出库数量

4. f                     ← 拍摄正视图（仅1个视图）
   （系统将自动比对并更新库存）

✅ 完成！库存已自动更新
   Removed: 5 | Remaining: 5
```

### 场景3：智能盘点（推荐）⭐
```bash
1. c                     ← 选择盘点模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入要盘点的MAC地址

3. 系统自动引导：
   [STEP 1/3] Please capture FRONT view
              Send 'f' to capture
   
   [STEP 2/3] Please capture SIDE view
              Send 's' to capture
   
   [STEP 3/3] Please capture TOP view
              Send 't' to capture and analyze

4. 自动输出分析报告：
   Weighted Confidence: 91.8745
   Threshold: 0.75
   ✅ MATCH - Same Asset
```

### 场景4：删除资产
```bash
1. d                     ← 选择删除模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入要删除的MAC地址

3. y                     ← 确认删除 (y/n)

✅ 完成！资产已从TF卡移除
```

### 场景5：查看已注册资产和存储空间
```bash
l                        ← 列出所有资产

显示:
=== Registered Assets ===
  [1] MAC: AA:BB:CC:DD:EE:FF | Item: Wooden Chair | Area: A | Stock: 10
  [2] MAC: 11:22:33:44:55:66 | Item: Metal Box    | Area: B | Stock: 5
Total: 2 assets

=== Storage Statistics ===
  Total Space: 7.5 GB
  Used Space:  150 KB
  Free Space:  7.5 GB
  Usage:       0.00%
  Status:      ✓ Healthy
========================
```

---

## ⚠️ 注意事项

### MAC地址格式
- ✅ 正确: `AA:BB:CC:DD:EE:FF`
- ❌ 错误: `AABBCCDDEEFF`（缺少冒号）
- ❌ 错误: `aa:bb:cc:dd:ee:ff`（建议大写）

### 拍摄提示
- 💡 保持稳定光照
- 📏 距离约30-50cm
- 🎯 物品居中
- 🖼️ 背景简洁

### 匹配判断说明
- **阈值**: 0.75（加权置信度）
- **权重分配**: 正面50%, 侧面30%, 顶部20%
- **可调参数**: 修改代码中的 `MATCH_THRESHOLD` 常量

### 常见错误
```
"Invalid MAC format"        → 检查MAC地址格式
"Please capture FRONT view first"  → 必须按顺序拍摄
"Storage initialization failed"    → 检查TF卡是否正确插入
"SD card is FULL"                  → 清理部分资产或更换更大容量TF卡
"Invalid quantity"                 → 请输入正整数
```

---

## 🔧 故障排除

| 问题 | 解决方法 |
|------|----------|
| 摄像头不启动 | 先输入有效的MAC地址 |
| TF卡初始化失败 | 检查TF卡是否插入，格式是否为FAT32 |
| 相似度始终很低 | 检查光照和拍摄角度是否一致 |
| 系统重启 | MobileNetV2推理耗时较长属正常现象 |
| TF卡空间不足 | 使用 `l` 查看后删除部分资产，或更换更大容量TF卡 |
| 出库失败 | 检查拍摄角度是否与注册时一致，或库存是否充足 |

---

## 📊 性能指标（V2.5）

- **MobileNetV2推理**: ~2.5秒/次
- **三视图注册总耗时**: ~25秒（含3帧融合）
- **完整盘点流程**: ~25秒（含三视图采集+分析）
- **出库完整流程**: **~7.5秒** ⭐NEW（仅拍摄正视图）
- **拍摄反馈延迟**: **~200ms** ⭐NEW（从~7.5秒优化）
- **每个资产存储**: ~15KB（包含详细信息）
- **匹配阈值**: 0.75（可调整）
- **TF卡写入速度**: ~500KB/s
- **8GB卡容量**: 约50万个资产（理论最大值）

---

## 📁 文件存储

```
/sdcard/assets/
├── AA_BB_CC_DD_EE_FF.dat    # 资产特征数据（三个1280维向量 + 元数据 + 详细信息）
└── ...
```

---

**文档版本**: V3.0  
**最后更新**: 2026-04-29  
**维护者**: ESP32-S3 CAM AI Team
