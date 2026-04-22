# ESP32-S3 CAM AI 资产管理系统使用指南

## 功能概述

本系统实现了基于MAC地址的资产管理功能，支持三视图（正面、侧面、顶部）拍照注册和盘点比对。

### 核心特性

1. **MAC地址管理**：通过串口输入MAC地址，验证通过后才启动摄像头
2. **WiFi流媒体开关**：默认关闭，可通过命令控制开启/关闭
3. **三视图拍摄**：引导用户依次拍摄三个视角
4. **双存储模式**：支持SD卡和内部Flash（SPIFFS）存储，可根据需要切换
5. **盘点比对**：支持三视图分别比对，提高识别准确性

---

## 快速开始

### 1. 硬件准备

- ESP32-S3开发板（带PSRAM）
- OV5640摄像头模块
- MicroSD卡（FAT32格式，可选）
- USB数据线

### 2. 编译烧录

```bash
# 设置目标芯片
idf.py set-target esp32s3

# 清理构建
idf.py fullclean

# 编译
idf.py build

# 烧录并监控（端口号根据实际情况修改）
idf.py flash monitor -p COM3
```

---

## 串口命令说明

### 基本命令

| 命令 | 说明 | 适用状态 |
|------|------|----------|
| `wifi on` | 开启WiFi视频流 | 任意状态 |
| `wifi off` | 关闭WiFi视频流 | 任意状态 |
| `XX:XX:XX:XX:XX:XX` | 输入MAC地址（如：AA:BB:CC:DD:EE:FF） | 等待MAC状态 |
| `r` 或 `R` | 重置系统，重新输入MAC | 任意状态 |

### 存储模式切换命令（新增）

| 命令 | 说明 |
|------|------|
| `storage sd` | 切换到SD卡存储模式 |
| `storage flash` | 切换到内部Flash（SPIFFS）存储模式 |
| `storage status` | 显示当前存储模式 |

### 资产注册命令（MAC地址输入后）

| 命令 | 说明 |
|------|------|
| `f` 或 `F` | 拍摄**正面**视图 |
| `s` 或 `S` | 拍摄**侧面**视图 |
| `t` 或 `T` | 拍摄**顶部**视图 |
| `l` 或 `L` | 列出所有已注册资产 |

### 资产盘点命令（三视图完成后）

| 命令 | 说明 |
|------|------|
| `c` 或 `C` | 进入盘点模式 |
| `1` | 拍摄并比对**正面**视图 |
| `2` | 拍摄并比对**侧面**视图 |
| `3` | 拍摄并比对**顶部**视图 |

---

## 使用流程

### 场景1：注册新资产

```
1. 系统启动后提示：Please input MAC address (format: XX:XX:XX:XX:XX:XX):

2. 输入物品标签上的MAC地址：
   AA:BB:CC:DD:EE:FF
   
3. 系统自动初始化存储（根据当前模式）和摄像头，提示：
   === Asset Registration Mode ===
   Send 'f' to capture FRONT view
   Send 's' to capture SIDE view
   Send 't' to capture TOP view
   
4. 按顺序拍摄三视图：
   - 发送 'f'，将物品正面对准摄像头，等待提示"FRONT view captured successfully"
   - 发送 's'，将物品侧面对准摄像头，等待提示"SIDE view captured successfully"
   - 发送 't'，将物品顶面对准摄像头，等待提示"TOP view captured successfully"
   
5. 系统自动保存到当前存储介质，提示：
   === All three views completed! ===
   Asset registered. Send 'c' for inventory check
```

### 场景2：切换存储模式

```
1. 系统运行时，可以随时切换存储模式：

   发送 'storage sd' - 切换到SD卡模式
   响应：Storage switched to SD Card
   
   发送 'storage flash' - 切换到内部Flash模式
   响应：Storage switched to SPIFFS (Internal Flash)
   
   发送 'storage status' - 查看当前模式
   响应：Current storage mode: SD Card 或 SPIFFS (Internal Flash)
```

### 场景3：盘点已有资产

```
前提：已完成资产注册

1. 输入该资产的MAC地址：
   AA:BB:CC:DD:EE:FF
   
2. 如果之前已注册过，系统会提示：
   WARNING: MAC AA:BB:CC:DD:EE:FF already registered!
   Overwriting existing record...
   
3. 重新拍摄三视图（或直接进入盘点模式）

4. 发送 'c' 进入盘点模式：
   === Inventory Check Mode ===
   Please position the item and send:
     '1' - Capture FRONT for comparison
     '2' - Capture SIDE for comparison
     '3' - Capture TOP for comparison
     
5. 分别拍摄三个视角进行比对：
   - 发送 '1'，拍摄正面，显示相似度：FRONT similarity: 0.9234 [MATCH]
   - 发送 '2'，拍摄侧面，显示相似度：SIDE similarity: 0.8876 [MATCH]
   - 发送 '3'，拍摄顶部，显示相似度：TOP similarity: 0.9012 [MATCH]
   
6. 根据相似度判断是否为同一物品（阈值默认0.85）
```

### 场景4：查看已注册资产

```
1. 在任意状态下发送 'l' 命令

2. 系统显示：
   === Registered Assets (SD Card) === 或 === Registered Assets (SPIFFS) ===
     [1] MAC: AA:BB:CC:DD:EE:FF
     [2] MAC: 11:22:33:44:55:66
   Total: 2 assets
   ========================
```

### 场景5：开启/关闭WiFi视频流

```
1. 发送 'wifi on'
   响应：WiFi stream ON
   浏览器访问 http://<ESP32-IP>/ 查看实时视频

2. 发送 'wifi off'
   响应：WiFi stream OFF
   视频流停止，节省带宽和功耗
```

---

## 文件存储结构

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

## 注意事项

### 1. SD卡要求
- 必须使用FAT32格式的MicroSD卡
- 首次使用时会自动格式化（数据会丢失，请提前备份）
- 建议使用Class 10及以上速度的卡

### 2. SPIFFS内部Flash存储
- 无需外部SD卡，使用内部Flash存储
- 需要在分区表中预留空间（默认3MB）
- 适合临时使用或没有SD卡的情况
- 容量有限，适合少量资产存储

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

## 高级配置

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

## 技术架构

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
VIEW_TOP (注册完成，保存到当前存储介质)
    ↓ (发送c)
INVENTORY_MODE (盘点模式)
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

## 后续扩展方向

1. **原始JPG图片保存**：在注册时同时保存三张原始照片
2. **批量盘点模式**：连续扫描多个资产，自动生成盘点报告
3. **Web界面管理**：通过HTTP页面进行资产注册和管理
4. **云端同步**：将资产数据同步到服务器
5. **二维码集成**：结合esp-who组件实现二维码识别
6. **语音提示**：添加TTS语音播报操作引导

---

## 版本历史

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

## 技术支持

如有问题，请查看：
- ESP-IDF官方文档：https://docs.espressif.com/projects/esp-idf/
- ESP-DL文档：https://docs.espressif.com/projects/esp-dl/
- 项目README.md和TROUBLESHOOTING.md
