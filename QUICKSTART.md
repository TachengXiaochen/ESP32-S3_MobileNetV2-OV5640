# ESP32-S3 CAM AI - MobileNetV2 快速开始指南（V2.6）

## ⚠️ 重要提示：ESP-IDF版本要求

**在开始之前，请确认你的ESP-IDF版本 >= 5.3.0**

检查当前版本：
```bash
idf.py --version
```

如果显示 v5.1.x 或更低版本，请先升级ESP-IDF。

查看升级指南: [UPGRADE_ESP_IDF.md](UPGRADE_ESP_IDF.md)

---

## 🎯 项目简介

基于 **ESP32-S3 + MobileNetV2** 的智能资产管理系统，支持：
- ✅ **🔍 模糊度检测**：拉普拉斯方差算法自动过滤模糊图像 ⭐NEW V2.6
- ✅ **🚪 出库模式**：仅拍摄正视图快速比对，自动更新库存 ⭐NEW V2.5
- ✅ **📋 资产详细信息**：物品名称、存放区域、数量完整管理 ⭐NEW V2.5
- ✅ **🔀 双线程架构**：拍摄与推理分离，响应速度提升37倍 ⭐NEW V2.5
- ✅ **三视图加权盘点**（准确率 >98%）
- ✅ **智能置信度分析**
- ✅ **TF卡存储**（唯一模式）
- ✅ **模块化多任务架构**

---

## 📦 硬件准备

| 组件 | 规格要求 | 备注 |
|------|---------|------|
| **开发板** | ESP32-S3（带PSRAM） | 推荐8MB PSRAM |
| **摄像头** | OV5640模块 | 支持RGB565格式 |
| **存储** | **MicroSD/TF卡（必需）** | FAT32格式，建议≥8GB |
| **数据线** | USB Type-C | 用于烧录和串口通信 |

**重要提示**：系统仅支持 TF卡（MicroSD卡）存储，使用前请确保已插入格式化的 TF卡。

---

## 🔧 环境配置

### 必需软件
- **ESP-IDF**: v5.3.5 或更高版本
- **Python**: 3.8+
- **CMake**: 3.5+
- **Git**: 2.0+

### 检查ESP-IDF版本
```bash
idf.py --version
# 应输出: v5.3.5 或更高
```

---

## 🚀 编译与烧录

### 步骤1: 设置目标芯片
```bash
idf.py set-target esp32s3
```

### 步骤2: 清理构建（重要！）
```bash
idf.py fullclean
```

### 步骤3: 编译项目
```bash
idf.py build
```

### 步骤4: 烧录并监控
```bash
# Linux/Mac
idf.py flash monitor -p /dev/ttyUSB0

# Windows（替换COM端口号）
idf.py flash monitor -p COM3
```

---

## 💻 首次使用流程

### 1. 系统启动
看到以下提示表示启动成功：
```
I (...) boot: Loaded app from partition at offset 0x10000
I (...) mobilenet_wrapper: MobileNetV2 model initialized
[SYSTEM] ESP32-CAM AI System Ready

========== MAIN MENU ==========
  r - Register new asset (入库)
  o - Outbound asset (出库) ⭐NEW
  c - Inventory existing asset
  d - Delete asset
  l - List all assets
  i - System information
  help/? - Show this menu
================================
[GUIDE] Please select an option: 
```

### 2. 选择业务模式
```bash
# 注册新资产
r

# 或者出库资产（V2.5新功能）
o

# 或者盘点现有资产
c

# 或者删除资产
d
```

### 3. 输入MAC地址
```bash
AA:BB:CC:DD:EE:FF
```

等待系统自动完成：
- ✅ TF卡挂载
- ✅ 摄像头初始化
- ✅ MobileNetV2模型加载

### 4. 开始操作

#### 方式A：智能盘点（推荐）⭐
```
# 1. 选择盘点模式
c

# 2. 输入MAC地址
AA:BB:CC:DD:EE:FF

# 3. 系统引导拍摄
[STEP 1/3] Please capture FRONT view
         Send 'f' to capture

[STEP 2/3] Please capture SIDE view
         Send 's' to capture

[STEP 3/3] Please capture TOP view
         Send 't' to capture and analyze

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

#### 方式B：注册新资产（V2.5升级版）
```
# 1. 选择注册模式
r

# 2. 输入MAC地址
AA:BB:CC:DD:EE:FF

# 3. 输入物品名称
Wooden Chair

# 4. 输入存放区域（单个字母A-Z）
A

# 5. 输入数量（正整数）
10

# 6. 按顺序拍摄三视图
f  # 拍摄正面
s  # 拍摄侧面
t  # 拍摄顶部并保存

✅ REGISTRATION COMPLETE!
  Asset saved to SD card successfully.
```

#### 方式C：出库资产 ⭐NEW V2.5
```bash
# 1. 选择出库模式
o

# 2. 输入MAC地址
AA:BB:CC:DD:EE:FF

# 3. 系统显示资产信息
========== OUTBOUND MODE ==========
  MAC: AA:BB:CC:DD:EE:FF
  Item: Wooden Chair
  Area: A
  Stock: 10
===================================

# 4. 输入出库数量
5

# 5. 拍摄正视图（仅1个视图）
f

# 6. 系统自动比对并更新库存
✅ OUTBOUND COMPLETE!
  Removed: 5 | Remaining: 5
```

### 4. 查看存储状态
```bash
i  # 查看TF卡容量使用情况
l  # 列出所有已注册资产（显示完整信息）
help  # 查看所有可用命令
```

---

## 📊 性能指标

| 指标 | V2.4数值 | V2.5数值 | 备注 |
|------|---------|-----------|------|
| **单次推理时间** | ~2.5秒 | ~2.5秒 | MobileNetV2推理 |
| **完整盘点耗时** | ~25秒 | ~25秒 | 三视图×3帧 |
| **出库完整流程** | ❌ 不支持 | **~7.5秒** | ⭐ **新功能** |
| **拍摄反馈延迟** | ~7.5秒 | **~200ms** | ⭐ **37倍提升** |
| **特征向量维度** | 1280 | 1280 | MobileNetV2输出 |
| **识别准确率** | >95% | >95% | 加权综合+多帧融合 |
| **内存占用** | ~4MB | ~4MB | PSRAM使用 |
| **TF卡写入速度** | ~500KB/s | ~500KB/s | 取决于TF卡等级 |

---

## ❓ 常见问题

### Q1: 编译失败怎么办？
```bash
# 执行完全清理后重新编译
idf.py fullclean
idf.py build
```

### Q2: MAC地址输入无响应？
- 确认波特率为 **115200**
- 勾选串口助手的 **"发送新行"** 选项
- 检查TX/RX接线是否正确

### Q3: 摄像头初始化失败？
- 确认使用 **OV5640** 型号
- 检查GPIO接线（XCLK=15, SIOD=4, SIOC=5等）
- 查看日志中的 `Camera PID` 是否为 `0x5640`

### Q4: TF卡挂载失败？
- **确认TF卡已正确插入卡槽**
- 确认TF卡为 **FAT32** 格式
- 检查GPIO 39/38/40 接线
- 尝试更换TF卡（部分高速卡不兼容）
- 查看串口日志中的具体错误代码

### Q5: 出现LoadProhibited崩溃？
在 `idf.py menuconfig` 中降低PSRAM频率：
```
Component config → ESP PSRAM → SPI RAM speed → 40MHz
```

### Q6: 出库模式如何使用？（V2.5新增）
- 输入 `o` 进入出库模式
- 仅适用于已注册的资产
- 仅需拍摄正视图进行快速比对
- 系统自动更新库存数量
- 数量归零时资产将被自动删除

### Q7: 为什么列表显示的资产信息不完整？（V2.5新增）
- 旧版本注册的资产可能缺少详细信息
- 建议重新注册以获得完整信息（名称、区域、数量）
- 新版本会自动迁移旧格式数据（填充默认值）

详细排错请查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)

---

## 📚 下一步

- 📖 阅读 [USER_GUIDE.md](USER_GUIDE.md) 了解完整功能
- 🏗️ 查看 [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) 了解技术架构
- 🛠️ 参考 [BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md) 掌握编译技巧

---

**祝你使用愉快！** 🎉
