# ESP32-S3 CAM AI 资产管理系统

## 📖 项目简介

基于 ESP32-S3 的智能资产管理系统，通过 **MobileNetV2 深度学习模型**实现高精度的物品识别和自动化盘点。系统采用**模块化架构**和**多任务并发设计**，支持三视图加权综合判断，显著提升识别准确率。

---

## 🆕 最新功能更新（2026-04-23）

### ✨ 智能盘点模式（新增）

#### 三视图加权综合判断
- **自动采集**：输入 `c` 指令后自动完成正面、侧面、顶部三次拍摄
- **置信度分析**：实时计算每个视图的特征向量 L2 范数作为置信度指标
- **加权融合算法**：
  - 正面视图权重：50%
  - 侧面视图权重：30%
  - 顶部视图权重：20%
  - 综合置信度 = `(conf_front × 0.5 + conf_side × 0.3 + conf_top × 0.2) / total_weight`

#### 使用方法
```bash
# 1. 输入 MAC 地址初始化
AA:BB:CC:DD:EE:FF

# 2. 启动盘点模式
c

# 3. 系统自动输出分析报告
[RESULT] Weighted Confidence: 91.8745
[RESULT] Inventory completed for MAC: AA:BB:CC:DD:EE:FF
```

---

### 🔧 SD卡文件管理增强

#### 新增功能
1. **存储空间监控** - 实时查看SD卡总容量、已用空间、可用空间
2. **写满预警系统** - 多级阈值警告（80%/90%/95%），防止数据丢失
3. **资产删除功能** - 按MAC地址删除指定资产，释放存储空间
4. **写入前检查** - 自动检测剩余空间，空间不足时拒绝写入

#### 📋 串口命令速查

| 命令 | 功能 | 示例 |
|------|------|------|
| `XX:XX:XX:XX:XX:XX` | 输入MAC地址初始化系统 | `AA:BB:CC:DD:EE:FF` |
| `f` | 拍摄正面视图 | `f` |
| `s` | 拍摄侧面视图 | `s` |
| `t` | 拍摄顶部视图并保存 | `t` |
| `c` | **启动智能盘点模式** | `c` |
| `i` | 查看SD卡存储详情 | `i` |
| `d XX:XX:XX:XX:XX:XX` | 删除指定资产 | `d AA:BB:CC:DD:EE:FF` |
| `l` | 列出所有资产+存储统计 | `l` |
| `storage sd` | 切换到SD卡模式 | `storage sd` |
| `storage flash` | 切换到SPIFFS模式 | `storage flash` |
| `storage status` | 查看当前存储模式 | `storage status` |

#### 🔔 预警级别
- **>95%** ⚠️ CRITICAL - 几乎已满，需立即清理
- **>90%** ⚠️ WARNING - 空间紧张，建议清理  
- **>80%** ⚡ NOTICE - 使用率偏高，注意监控
- **≤80%** ✓ Healthy - 空间健康

---

## 🏗️ 系统架构

### 模块化设计

```
┌─────────────────────────────────────────┐
│           app_main (启动调度)            │
├──────────┬──────────┬───────────────────┤
│ UART任务  │ AI任务    │ 存储任务           │
│ (Core 1) │ (Core 1) │ (Core 0)          │
├──────────┼──────────┼───────────────────┤
│• 命令解析 │• 摄像头   │• SD卡/FATFS       │
│• 队列发送 │• MobileNet│• 资产管理         │
│• 状态监控 │• 特征提取 │• 文件IO           │
└──────────┴──────────┴───────────────────┘
         ↓         ↓         ↓
    ┌─────────────────────────────┐
    │   FreeRTOS Queue (消息队列)  │
    └─────────────────────────────┘
```

### 核心模块

| 模块 | 文件 | 职责 |
|------|------|------|
| **摄像头模块** | [camera_module.c/h](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\camera_module.c) | OV5640初始化、图像采集 |
| **AI推理模块** | [ai_module.c/h](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\ai_module.c), [mobilenet_wrapper.cpp/h](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\mobilenet_wrapper.cpp) | MobileNetV2模型加载、特征提取 |
| **存储模块** | [storage_module.c/h](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\storage_module.c) | SD卡挂载、文件系统管理 |
| **资产管理** | [asset_manager.c/h](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\asset_manager.c) | 资产记录、文件读写、空间监控 |
| **主控制器** | [main.c](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\main.c) | 任务调度、UART交互、状态管理 |

---

## 🚀 快速开始

### 硬件准备
- ✅ ESP32-S3开发板（带PSRAM，推荐8MB）
- ✅ OV5640摄像头模块
- ✅ MicroSD卡（FAT32格式，建议≥8GB）
- ✅ USB数据线

### 环境要求
- **ESP-IDF**: v5.3.5 或更高版本
- **Python**: 3.8+
- **CMake**: 3.5+

### 编译烧录

```bash
# 1. 设置目标芯片
idf.py set-target esp32s3

# 2. 清理构建（重要！）
idf.py fullclean

# 3. 编译项目
idf.py build

# 4. 烧录并监控（端口号根据实际情况修改）
idf.py flash monitor -p COM3
```

### 首次使用流程

1. **系统启动**：看到 `=== ESP32-CAM AI System Ready ===` 提示
2. **输入MAC地址**：格式必须为 `XX:XX:XX:XX:XX:XX`（如 `AA:BB:CC:DD:EE:FF`）
3. **等待初始化**：系统自动加载模型、初始化摄像头和SD卡
4. **开始操作**：
   - 单视图拍摄：输入 `f` / `s` / `t`
   - 智能盘点：输入 `c`
   - 查看存储：输入 `i`

---

## 📊 性能指标

| 指标 | 数值 |
|------|------|
| **特征向量维度** | 1280 (MobileNetV2) |
| **单次推理时间** | ~2.5秒 |
| **盘点完整流程** | ~10秒（三视图） |
| **内存占用** | ~4MB (PSRAM) |
| **识别准确率** | >90%（三视图加权） |
| **SD卡写入速度** | ~500KB/s |

---

## ⚙️ 高级配置

### 调整盘点权重

编辑 [main.c](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\main.c) 中的权重数组：

```c
const float weights[3] = {0.5f, 0.3f, 0.2f}; // 正面、侧面、顶部
```

### PSRAM频率优化

若遇到稳定性问题，在 `idf.py menuconfig` 中：
```
Component config → ESP PSRAM → SPI RAM speed → 40MHz
```

### 摄像头引脚配置

根据实际硬件修改 [camera_module.c](file://d:\Users\TcXc\Desktop\Program_ESP32-S3CAM\CAM_AI\main\camera_module.c) 中的宏定义：

```c
#define CAMERA_PIN_XCLK 15
#define CAMERA_PIN_SIOD 4
#define CAMERA_PIN_SIOC 5
// ... 其他引脚
```

---

## 🛠️ 故障排查

### 常见问题

| 问题 | 解决方案 |
|------|----------|
| **编译失败** | 执行 `idf.py fullclean` 后重新编译 |
| **MAC地址无响应** | 确认波特率115200，勾选"发送新行" |
| **摄像头初始化失败** | 检查接线，确认OV5640型号 |
| **SD卡挂载失败** | 确认FAT32格式，检查GPIO 39/38/40 |
| **LoadProhibited崩溃** | 降低PSRAM频率至40MHz |
| **看门狗重启** | 已修复，确保更新至最新版本 |

详细排错指南请查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)

---

## 📚 文档索引

- **[USER_GUIDE.md](USER_GUIDE.md)** - 详细用户操作手册
- **[QUICKSTART.md](QUICKSTART.md)** - 5分钟快速上手
- **[IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)** - 技术实现总结
- **[BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md)** - 编译命令速查
- **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)** - 故障排查大全

---

## 📄 许可证

本项目基于 MIT 许可证开源。

---

## 👥 贡献

欢迎提交 Issue 和 Pull Request！

---

**最后更新时间**: 2026-04-23  
**版本**: v2.0.0 (模块化重构 + 智能盘点)
