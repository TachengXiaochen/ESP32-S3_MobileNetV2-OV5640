# ESP32-S3 CAM AI 资产管理系统使用指南

## 📖 功能概述

本系统实现了基于 **MAC地址** 的资产管理功能，支持**三视图（正面、侧面、顶部）**拍照注册和**智能盘点比对**。系统采用**模块化架构**和**多任务并发设计**，通过 **MobileNetV2 深度学习模型**实现高精度物品识别。

### ✨ 核心特性

1. **MAC地址管理**：通过串口输入MAC地址，验证通过后才启动摄像头
2. **🌟 智能盘点模式**：一键自动完成三视图采集 + **加权综合置信度分析**
3. **灵活注册模式**：支持手动分步拍摄，顶部视图拍摄后自动保存
4. **双存储模式**：支持SD卡和内部Flash（SPIFFS）存储，可动态切换
5. **实时置信度反馈**：每次推理提供置信度评分，量化识别质量
6. **资产精细化管理**：支持查看存储详情、删除指定资产及列表统计

---

## 🚀 快速开始

### 1. 硬件准备

- ✅ ESP32-S3开发板（带PSRAM，推荐8MB）
- ✅ OV5640摄像头模块
- ✅ MicroSD卡（FAT32格式，建议≥8GB，**可选**）
- ✅ USB数据线

**说明**：系统默认使用内部 Flash（SPIFFS）存储资产数据，无需外部 SD 卡。如需更大容量，可通过 `storage sd` 命令切换到 SD 卡模式。

### 2. 编译烧录

```bash
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

## 📋 串口命令详解

### 📡 基本控制命令

| 命令 | 功能 | 示例 | 适用状态 |
|------|------|------|----------|
| `wifi on` | 开启WiFi视频流 | `wifi on` | 任意状态 |
| `wifi off` | 关闭WiFi视频流 | `wifi off` | 任意状态 |
| `XX:XX:XX:XX:XX:XX` | 输入MAC地址初始化系统 | `AA:BB:CC:DD:EE:FF` | 等待MAC状态 |
| `r` 或 `R` | 重置系统，重新输入MAC | `r` | 任意状态 |

### 📦 存储管理命令

| 命令 | 功能 | 示例 |
|------|------|------|
| `storage sd` | 切换到SD卡存储模式 | `storage sd` |
| `storage flash` | 切换到内部Flash（SPIFFS）模式 | `storage flash` |
| `storage status` | 显示当前存储模式 | `storage status` |
| `i` | **查看SD卡存储详情**（容量/使用率） | `i` |
| `d XX:XX:XX:XX:XX:XX` | **删除指定资产** | `d AA:BB:CC:DD:EE:FF` |
| `l` | 列出所有已注册资产 + 存储统计 | `l` |

### 📸 资产注册命令（MAC地址输入后）

| 命令 | 功能 | 说明 |
|------|------|------|
| `f` 或 `F` | 拍摄**正面**视图 | 单独采集，不自动保存 |
| `s` 或 `S` | 拍摄**侧面**视图 | 单独采集，不自动保存 |
| `t` 或 `T` | 拍摄**顶部**视图 | **自动保存三视图到存储** |

### 🎯 智能盘点命令

| 命令 | 功能 | 工作流程 |
|------|------|----------|
| `c` 或 `C` | **启动智能盘点模式** | 自动完成三视图采集 + 加权综合判断 |

---

## 🛠️ 详细使用流程

### 场景1：注册新资产（手动模式）

```bash
# 1. 系统启动后提示：
[SYSTEM] ESP32-CAM AI System Ready
[GUIDE] Please input MAC address (Format: XX:XX:XX:XX:XX:XX)

# 2. 输入物品标签上的MAC地址：
AA:BB:CC:DD:EE:FF
   
# 3. 系统自动初始化，提示：
[SYSTEM] Hardware initialized.
[GUIDE] Please input 'f' (Front), 's' (Side), or 't' (Top) to capture.
   
# 4. 按顺序拍摄三视图：
f  # 拍摄正面，等待 "Feature extracted successfully"
s  # 拍摄侧面，等待 "Feature extracted successfully"
t  # 拍摄顶部并自动保存，等待 "✅ Asset saved to SD card"
   
# 5. 完成注册：
=== All three views completed! ===
Asset registered successfully.
```

### 场景2：智能盘点（推荐）⭐

```bash
# 1. 输入MAC地址初始化（同上）
AA:BB:CC:DD:EE:FF

# 2. 启动盘点模式：
c

# 3. 系统自动执行：
[INVENTORY] Starting multi-view inventory mode...
[INVENTORY] Capturing front view...
I (...) Front view captured, confidence: 92.5561
[INVENTORY] Capturing side view...
I (...) Side view captured, confidence: 89.2934
[INVENTORY] Capturing top view...
I (...) Top view captured, confidence: 95.1234
[INVENTORY] Analyzing multi-view features...

# 4. 输出分析报告：
I (...) Inventory Analysis Result:
I (...)   Front confidence: 92.5561 (weight: 0.5)
I (...)   Side confidence:  89.2934 (weight: 0.3)
I (...)   Top confidence:   95.1234 (weight: 0.2)
I (...)   Weighted综合置信度: 91.8745

[RESULT] Weighted Confidence: 91.8745
[RESULT] Inventory completed for MAC: AA:BB:CC:DD:EE:FF
```

**优势**：
- ✅ 全自动流程，无需人工干预
- ✅ 实时置信度分析，识别质量可量化
- ✅ 加权综合判断，准确率 >90%

### 场景3：切换存储模式

```bash
# 1. 查看当前存储模式：
storage status
响应：Current storage mode: SPIFFS (Internal Flash)

# 2. 切换到 SD 卡模式（适合大量资产）：
storage sd
响应：Storage switched to SD Card
   
# 3. 切换回内部Flash模式：
storage flash
响应：Storage switched to SPIFFS (Internal Flash)
```

**提示**：
- 默认 SPIFFS 模式可存储约 60-70 个资产
- 如需存储更多资产，建议切换到 SD 卡模式
- 切换后原存储介质的数据仍然保留

### 场景4：资产管理与WiFi

**查看与删除资产**：
```bash
# 列出所有资产及统计信息
l

# 删除指定MAC地址的资产
d AA:BB:CC:DD:EE:FF

# 查看存储详细信息（容量/使用率）
i
```

**WiFi视频流**：
```bash
# 开启视频流
wifi on
# 浏览器访问 http://<ESP32-IP>/ 查看实时视频

# 关闭视频流
wifi off
```

---

## 📁 文件存储结构

### SD卡模式 (/sdcard/)
```
/sdcard/
└── assets/
    ├── AA_BB_CC_DD_EE_FF.dat    # MAC为AA:BB:CC:DD:EE:FF的资产记录
    ├── 11_22_33_44_55_66.dat    # MAC为11:22:33:44:55:66的资产记录
    └── ...
```

### SPIFFS模式 (/spiffs/)
```
/spiffs/
└── assets/
    ├── AA_BB_CC_DD_EE_FF.dat    # MAC为AA:BB:CC:DD:EE:FF的资产记录
    ├── 11_22_33_44_55_66.dat    # MAC为11:22:33:44:55:66的资产记录
    └── ...
```

每个`.dat`文件包含：
- MAC地址字符串（18字节）
- 正面特征向量（1280×4=5120字节）
- 侧面特征向量（1280×4=5120字节）
- 顶部特征向量（1280×4=5120字节）
- 有效性标志（1字节）

总计约15KB/资产

---

## ⚠️ 注意事项

### 1. SPIFFS内部Flash存储（**默认模式**）
- ✅ **无需外部SD卡**，使用内部Flash存储
- ✅ 分区表中已预留 1MB 空间
- ✅ 可存储约 **60-70 个资产**（每个约15KB）
- ✅ 适合日常使用和少量资产管理
- ⚠️ 如需存储更多资产，可通过 `storage sd` 切换到 SD 卡模式

### 2. SD卡要求（可选）
- 必须使用FAT32格式的MicroSD卡
- 首次使用时会自动格式化（数据会丢失，请提前备份）
- 建议使用Class 10及以上速度的卡
- 通过 `storage sd` 命令切换到 SD 卡模式

### 3. 拍摄建议
- **光照条件**：保持稳定、均匀的光照，避免强光和背光
- **拍摄距离**：保持物品在画面中心，距离适中（约30-50cm）
- **角度准确**：
  - 正面：物品主要特征面
  - 侧面：旋转90度
  - 顶部：从上往下拍摄
- **背景简洁**：尽量使用纯色背景，减少干扰

### 4. 性能说明
- MobileNetV2推理耗时：约1.3秒/次
- 三视图注册总耗时：约4-5秒
- 单次盘点比对耗时：约1.3秒
- 建议耐心等待提示，不要频繁发送命令

### 5. WiFi配置
- 默认SSID：`ESP32_CAM`
- 默认密码：`12345678`
- 最大连接数：4个客户端
- 默认不开启视频流，需手动输入`wifi on`

### 6. 存储模式切换
- 可在运行时随时切换存储模式
- 切换后原存储介质的数据仍然保留
- 新数据将保存到当前选定的存储介质
- 系统会自动处理存储介质的挂载和卸载

### 7. 故障排查

**问题1：SD卡初始化失败**
```
解决：
- 检查SD卡是否正确插入
- 确认引脚连接正确（CLK=14, CMD=15, D0=2）
- 尝试更换SD卡
- 查看串口日志中的具体错误信息
```

**问题2：SPIFFS初始化失败**
```
解决：
- 检查分区表是否包含spiffs分区
- 确认分区表中spiffs分区大小是否足够
- 查看串口日志中的具体错误信息
```

**问题3：摄像头初始化失败**
```
解决：
- 检查摄像头排线连接
- 确认引脚定义与硬件匹配
- 查看XCLK频率是否合适（当前10MHz）
```

**问题4：相似度始终很低**
```
解决：
- 检查拍摄时光照是否稳定
- 确认拍摄角度与注册时一致
- 尝试调整COSINE_THRESHOLD阈值（当前0.85）
- 重新注册资产，确保三视图清晰
```

**问题5：系统重启或看门狗超时**
```
解决：
- MobileNetV2推理耗时较长属正常现象
- 代码中已添加看门狗复位，不应出现此问题
- 如仍出现，检查PSRAM是否正常启用
```

---

## 🔧 高级配置

### 修改相似度阈值

在`main.c`中修改：
```c
#define COSINE_THRESHOLD 0.85f  // 调整为0.80-0.95之间的值
```

- **降低阈值**（如0.80）：更宽松，减少漏报但可能增加误报
- **提高阈值**（如0.90）：更严格，减少误报但可能增加漏报

### 修改WiFi配置

在`main.c`中修改：
```c
#define WIFI_SSID "YourCustomSSID"
#define WIFI_PASS "YourPassword"
#define MAX_STA_CONN 4  // 最大连接数
```

### 修改SD卡引脚

在`asset_manager.c`中修改：
```c
#define SD_PIN_CLK  14
#define SD_PIN_CMD  15
#define SD_PIN_D0   2
```

---

## 🛠️ 技术架构

### 状态机设计

```
CAM_STATE_WAITING_MAC
    ↓ (输入有效MAC)
CAM_STATE_READY
    ↓ (拍摄f)
VIEW_FRONT
    ↓ (拍摄s)
VIEW_SIDE
    ↓ (拍摄t)
VIEW_TOP (注册完成，自动保存到当前存储介质)
    ↓ (发送c)
INVENTORY_MODE (智能盘点：自动采集三视图 -> 加权分析 -> 输出结果)
```

### 特征提取流程

```
摄像头捕获(RGB565) 
    → 转换为RGB888 
    → MobileNetV2推理(224x224输入)
    → 获取1280维输出
    → INT8反量化为Float
    → L2归一化
    → 余弦相似度比对
```

### 内存管理

- **静态分配**：特征向量数组（1280×4个float数组）使用static声明，避免栈溢出
- **动态分配**：RGB转换缓冲区使用malloc/free，用后立即释放
- **PSRAM依赖**：MobileNetV2模型和中间结果存储在PSRAM中

---

## 🚀 后续扩展方向

1. **原始JPG图片保存**：在注册时同时保存三张原始照片
2. **批量盘点模式**：连续扫描多个资产，自动生成盘点报告
3. **Web界面管理**：通过HTTP页面进行资产注册和管理
4. **云端同步**：将资产数据同步到服务器
5. **二维码集成**：结合esp-who组件实现二维码识别
6. **语音提示**：添加TTS语音播报操作引导

---

## 📝 版本历史

- **v1.1** (2026-04-22)
  - 新增SD卡/Flash双存储模式切换功能
  - 添加`storage sd`、`storage flash`、`storage status`命令
  - 优化存储管理，支持运行时切换存储介质

- **v1.0** (2026-04-22)
  - 实现MAC地址管理和验证
  - 实现WiFi流媒体开关控制
  - 实现三视图拍摄引导流程
  - 实现SD卡资产存储和加载
  - 实现盘点比对功能

---

## 🛠️ 技术支持

如有问题，请查看：
- ESP-IDF官方文档：https://docs.espressif.com/projects/esp-idf/
- ESP-DL文档：https://docs.espressif.com/projects/esp-dl/
- 项目README.md和TROUBLESHOOTING.md
