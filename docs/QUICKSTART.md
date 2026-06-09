# ESP32-S3 CAM AI - 快速开始指南 v3.5

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
- ✅ **🆔 Tag ID标识** ⭐v3.2：16位十六进制唯一标识（0x0001-0xFFFF）
- ✅ **✅ 验证式更新** ⭐v3.2：Tag ID已存在时验证身份后累加数量
- ✅ **⚡ 极速推理** ⭐v3.5：三视图全流程从30-60s降至8.5s（降低80-85%）
- ✅ **🎯 GAP 1280维特征** ⭐v3.5：替代1000维logits，语义表达能力更强
- ✅ **🔍 纯余弦相似度** ⭐v3.5：移除无效Euclidean混合，阈值统一0.90
- ✅ **🚪 出库模式（分步控制）** ⭐v3.4：按需初始化硬件，用户确认后再拍照
- ✅ **📋 资产详细信息** ⭐v2.5：物品名称、存放区域、数量完整管理
- ✅ **🔀 双线程架构** ⭐v2.5：拍摄与推理分离，响应速度提升37倍
- ✅ **🏗️ business_executor架构** ⭐v3.4：统一业务逻辑处理，双通道输出
- ✅ **三视图加权盘点**（准确率 >98%）
- ✅ **智能置信度分析**
- ✅ **TF卡存储**（唯一模式）
- ✅ **模块化多任务架构**

**维护者**: TcXc  
**远程仓库**: 
- GitHub: [ESP32-S3_MobileNetV2-OV5640](https://github.com/TachengXiaochen/ESP32-S3_MobileNetV2-OV5640)
- Gitee镜像: [ESP32-S3_MobileNetV2-OV5640](https://gitee.com/star-flash-smart-inventory/ESP32-S3_MobileNetV2-OV5640)
**项目主页**: [星闪智能盘点系统](https://gitee.com/star-flash-smart-inventory)  
**WS63端仓库**: [ws63_bs2x_sle_project](https://gitee.com/star-flash-smart-inventory/ws63_bs2x_sle_project) (主要负责人)  
**问题反馈**: 
- GitHub Issues: [GitHub Issues](https://github.com/TachengXiaochen/ESP32-S3_MobileNetV2-OV5640/issues)
- Gitee Issues: [Gitee Issues](https://gitee.com/star-flash-smart-inventory/ESP32-S3_MobileNetV2-OV5640/issues)  
**反馈邮箱**: 202500201056@stumail.sztu.edu.cn  

**祝你使用愉快！** 🎉

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

| 指标 | v3.4数值 | v3.5数值 | 备注 |
|------|---------|-----------|------|
| **三视图推理时间** | ~30-60秒 | **~8.5秒** | ⭐ **降低80-85%** |
| **每视图帧数** | 3帧固定 | 1帧（边缘3帧） | ⭐ **自适应策略** |
| **特征维度** | 1000 logits | **1280 GAP** | ⭐ **语义特征** |
| **同物品cosine** | ~1.0 (bug) | **>0.85** | ⭐ **修复覆盖bug** |
| **异物品cosine** | ~1.0 (bug) | **0.55-0.65** | ⭐ **区分度提升** |
| **匹配算法** | 70%cosine+30%euclidean | **100% cosine** | ⭐ **简化有效** |
| **匹配阈值** | 0.70-0.85 | **0.90** | ⭐ **统一提升** |
| **单次拍摄时间** | ~800ms | ~600ms | 模糊检测降采样 |
| **特征提取时间** | ~1.2s | ~0.9s | 移除冗余计算 |
| **相似度计算** | <10ms | <10ms | 纯余弦更高效 |
| **出库完整流程** | ❌ 不支持 | **~7.5秒** | ⭐ **新功能** |
| **拍摄反馈延迟** | ~7.5秒 | **~200ms** | ⭐ **37倍提升** |
| **内存占用** | ~4MB | ~4MB | PSRAM使用 |
| **TF卡写入速度** | ~500KB/s | ~500KB/s | 取决于TF卡等级 |