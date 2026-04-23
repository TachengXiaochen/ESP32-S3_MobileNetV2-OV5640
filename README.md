# ESP32-S3 CAM AI 资产管理系统

## 📋 功能概述

本项目实现了基于ESP32-S3的智能资产管理系统，通过深度学习特征提取实现高精度的物品识别和盘点。

### ✨ 核心功能

1. **MAC地址管理** - 通过串口输入MAC地址，验证通过后才启动摄像头
2. **WiFi流媒体开关** - 默认关闭，通过`wifi on/off`命令灵活控制
3. **三视图拍摄** - 引导用户依次拍摄正面、侧面、顶部三个视角
4. **双存储模式** - 支持SD卡和内部Flash（SPIFFS）存储，可运行时切换（**默认使用SPIFFS**）
5. **盘点比对** - 支持三视图分别比对，提高识别准确性
6. **MobileNetV2深度学习** - 使用预训练模型提取1280维高区分度特征

---

## 🚀 快速开始

### 硬件准备
- ✅ ESP32-S3开发板（带PSRAM）
- ✅ OV5640摄像头模块
- ✅ MicroSD卡（FAT32格式，可选）
- ✅ USB数据线

### 编译烧录

```bash
# 1. 设置目标芯片
idf.py set-target esp32s3

# 2. 清理构建
idf.py fullclean

# 3. 编译项目
idf.py build

# 4. 烧录并监控（端口号根据实际情况修改）
idf.py flash monitor -p COM3
```

### 首次使用

系统启动后会提示：
```
=== ESP32-CAM AI Asset Management System ===
Please input MAC address (format: XX:XX:XX:XX:XX:XX):
Commands:
  wifi on/off - Enable/disable WiFi stream
```

**详细使用说明请查看 [USER_GUIDE.md](USER_GUIDE.md)**

---

## 📖 使用指南

### 资产管理流程（新功能）

#### 1️⃣ 输入MAC地址

系统启动后，首先输入物品标签上的MAC地址：

```
Please input MAC address (format: XX:XX:XX:XX:XX:XX):
AA:BB:CC:DD:EE:FF
```

系统会自动：
- ✅ 验证MAC地址格式
- ✅ 初始化SD卡
- ✅ 启动摄像头
- ✅ 检查该MAC是否已注册

#### 2️⃣ 拍摄三视图

系统提示进入注册模式后，按顺序拍摄三个视角：

```
=== Asset Registration Mode ===
Send 'f' to capture FRONT view
Send 's' to capture SIDE view
Send 't' to capture TOP view
```

**操作步骤：**
1. 发送 `f` - 拍摄**正面**视图
2. 发送 `s` - 拍摄**侧面**视图  
3. 发送 `t` - 拍摄**顶部**视图

每步都有明确的引导提示，系统会自动保存特征到SD卡。

#### 3️⃣ 盘点比对

完成三视图注册后，可以开始盘点：

```
发送 'c' 进入盘点模式
然后分别发送：
  '1' - 比对正面视图
  '2' - 比对侧面视图
  '3' - 比对顶部视图
```

系统会显示每个视角的相似度：
```
FRONT similarity: 0.9234 [MATCH]
SIDE similarity: 0.8876 [MATCH]
TOP similarity: 0.9012 [MATCH]
```

#### 4️⃣ 存储模式切换（新增）

系统支持两种存储模式，可运行时切换：

```
storage sd      - 切换到SD卡存储模式
storage flash   - 切换到内部Flash（SPIFFS）模式（**默认**）
storage status  - 查看当前存储模式
```

**使用场景：**
- **SPIFFS模式（默认）**：无需外部SD卡，适合日常使用和少量资产（约60-70个）
- **SD卡模式**：大容量存储，适合大量资产管理

#### 5️⃣ 其他命令

- **`wifi on`** - 开启WiFi视频流（默认关闭）
- **`wifi off`** - 关闭WiFi视频流
- **`l`** - 列出所有已注册的资产
- **`r`** - 重置系统，重新输入MAC地址

---

### WiFi视频流（传统功能）

如需查看实时视频：

1. 发送 `wifi on` 开启视频流
2. 连接到WiFi热点 `ESP32_CAM`（密码：12345678）
3. 浏览器访问 `http://192.168.4.1/`
4. 页面将显示实时视频流
5. 使用完毕后发送 `wifi off` 关闭以节省功耗

---

## ⚠️ 重要：ESP-IDF版本要求

**本项目需要 ESP-IDF >= 5.3.0**

当前你使用的是 **ESP-IDF v5.1.2**，需要升级才能使用esp-dl的MobileNetV2功能。

### 快速升级（Windows）

1. **下载ESP-IDF v5.3+安装器**
   - 访问: https://dl.espressif.com/dl/esp-idf/
   - 下载最新的在线安装器

2. **安装新版本**
   - 运行安装器，选择v5.3.x或更高版本
   - 等待安装完成（约30分钟）

3. **切换版本**
   ```
   VSCode → Ctrl+Shift+P → "ESP-IDF: Select ESP-IDF version to use"
   → 选择新安装的v5.3.x
   ```

4. **重新编译**
   ```bash
   idf.py fullclean
   idf.py build
   ```

详细升级指南请查看: [UPGRADE_ESP_IDF.md](UPGRADE_ESP_IDF.md)

### 为什么需要升级？

- esp-dl v3.3.1 (当前使用的版本) 要求 ESP-IDF >= 5.3
- v5.3提供了更好的性能和稳定性
- 支持最新的AI模型和优化

---

## 📋 系统要求

- ESP32-S3开发板
- OV5640或其他兼容摄像头模块
- PSRAM（必需，用于MobileNetV2模型）

## 引脚配置

```c
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_V_SYNC 6
#define CAM_PIN_H_SYNC 7
#define CAM_PIN_PCLK   13
```

## WiFi配置

- SSID: `ESP32_CAM`
- 密码: `12345678`
- 最大连接数: 4

## 使用方法

### 1. 查看实时视频流

1. 烧录固件到ESP32-S3
2. 连接到WiFi热点 `ESP32_CAM`（密码：12345678）
3. 在浏览器中打开 `http://192.168.4.1/`
4. 页面将显示实时视频流

### 2. 特征提取与比对（MobileNetV2）

通过串口发送以下命令（波特率115200）：

- **命令 `1`**: 拍摄第一张照片并提取MobileNetV2特征向量
  - 响应: "MobileNetV2 Feature 1 stored" 或 "MobileNetV2 Feature 1 failed"
  - 耗时: 约1.3秒（首次可能更长，需要加载模型）
  
- **命令 `2`**: 拍摄第二张照片，提取特征并与第一张比对
  - 响应: 显示余弦相似度和判断结果
  - 相似度 >= 0.85: "Same object! MobileNetV2 Similarity: X.XXXX"
  - 相似度 < 0.85: "Different objects! MobileNetV2 Similarity: X.XXXX"
  - 耗时: 约1.3秒
  
- **命令 `r` 或 `R`**: 重置已存储的特征
  - 响应: "Feature reset"

### 3. 调整相似度阈值

在 `main.c` 中修改：
```c
#define COSINE_THRESHOLD 0.85f  // 调整为合适的值 (0.0-1.0)
```

**阈值建议**:
- 0.90-0.95: 非常严格，适合精确匹配
- 0.80-0.90: 适中，平衡准确率和召回率（推荐）
- 0.70-0.80: 较宽松，适合相似物体检测

## MobileNetV2特征提取算法

### 技术架构

```
摄像头捕获(RGB565, 96x96)
         ↓
    转换为RGB888
         ↓
  MobileNetV2推理
  (INT8量化模型)
         ↓
  获取最后一层输出
    (1280维向量)
         ↓
    反量化处理
  (INT8 → Float)
         ↓
    L2归一化
         ↓
  余弦相似度比对
```

### 模型信息

- **模型名称**: MobileNetV2 (imagenet_cls_mobilenetv2_s8_v1.espdl)
- **量化精度**: INT8对称量化
- **输入尺寸**: 224x224x3 (模型内部自动resize)
- **特征维度**: 1280维
- **预处理**: mean=[123.675, 116.28, 103.53], std=[58.395, 57.12, 57.375]
- **存储位置**: components/esp-dl/models/imagenet_cls/models/s3/

### 性能对比

| 特性 | MobileNetV2 (新) | 手工特征 (旧) |
|------|------------------|--------------|
| 特征维度 | 1280维 | 1024维 |
| 提取速度 | ~1291ms | ~100-200ms |
| **准确性** | **高 ⭐⭐⭐⭐⭐** | 中等 ⭐⭐⭐ |
| **泛化能力** | **强 ⭐⭐⭐⭐⭐** | 弱 ⭐⭐ |
| **光照鲁棒性** | **优秀 ⭐⭐⭐⭐⭐** | 较好 ⭐⭐⭐⭐ |
| **旋转/缩放鲁棒性** | **优秀 ⭐⭐⭐⭐⭐** | 一般 ⭐⭐⭐ |
| 内存占用 | 较高 (需PSRAM) | 低 |
| 实现复杂度 | 低(封装好) | 高 |

## 性能优化建议

### 提高准确性

1. **环境条件**:
   - 在稳定光照条件下采集样本
   - 避免强烈背光或阴影
   - 保持背景简洁

2. **拍摄技巧**:
   - 保持物体在画面中心
   - 确保物体清晰对焦
   - 避免快速移动造成模糊
   - 尽量保持相同角度和距离

3. **参数调整**:
   - 根据实际场景调整COSINE_THRESHOLD
   - 多次采样取平均特征值
   - 为同一物体采集多个角度的特征

### 提高速度

1. **硬件升级**:
   - 使用ESP32-P4芯片（推理速度快3-4倍，约350ms）
   - 增加PSRAM容量

2. **软件优化**:
   - 降低摄像头分辨率（当前96x96已是最小）
   - 预初始化模型减少首次延迟
   - 考虑使用更小的模型（如MobileNetV2-0.5）

### 内存优化

1. **启用PSRAM**:
   ```
   idf.py menuconfig
   → Component config → ESP PSRAM → Enable
   ```

2. **优化配置**:
   - 减少帧缓冲区数量（当前为2）
   - 使用JPEG格式传输而非RGB565

## 故障排除

### 问题1: 浏览器看不到视频流

**可能原因**:
- WiFi未正确连接
- HTTP服务器启动失败
- 摄像头初始化失败

**解决方法**:
1. 检查串口日志，确认WiFi AP已启动
2. 确认摄像头初始化成功
3. 尝试刷新浏览器页面
4. 检查IP地址是否正确（通常是192.168.4.1）

### 问题2: MobileNetV2模型加载失败

**可能原因**:
- 缺少PSRAM
- CMakeLists.txt未添加依赖
- 模型文件损坏

**解决方法**:
1. 确认已启用PSRAM支持
2. 检查CMakeLists.txt包含`imagenet_cls`依赖
3. 重新编译并烧录固件
4. 查看ESP_LOGE日志确认具体错误

### 问题3: 特征比对准确性不理想

**可能原因**:
- 光照条件变化大
- 拍摄角度差异大
- 物体移动或模糊
- 阈值设置不合适

**解决方法**:
1. 在稳定光照下采集样本
2. 保持拍摄角度一致
3. 确保物体清晰对焦
4. 调整COSINE_THRESHOLD阈值（建议0.80-0.90）
5. 为同一物体采集多个样本取平均

### 问题4: 推理速度慢

**说明**: 
- ESP32-S3上MobileNetV2推理约需1.3秒是正常现象
- 这是深度学习模型的固有特性

**优化方案**:
1. 接受当前速度（适用于非实时应用）
2. 升级到ESP32-P4（速度提升至~350ms）
3. 如需实时处理，考虑使用专用AI加速芯片

### 问题5: 串口无响应

**可能原因**:
- 波特率设置错误
- UART任务未启动
- 栈溢出

**解决方法**:
1. 确认串口波特率为115200
2. 检查串口日志输出
3. 增加UART任务的栈大小（当前已设置为8192）

## 技术细节

### HTTP流媒体实现

- 使用MJPEG格式（multipart/x-mixed-replace）
- 每帧转换为JPEG格式传输
- 自动处理RGB565到JPEG的转换
- 支持多客户端同时访问

### MobileNetV2特征比对

使用余弦相似度计算两个特征向量的相似性：

```
similarity = (A·B) / (||A|| * ||B||)
```

由于特征向量已进行L2归一化，公式简化为：
```
similarity = A·B （点积）
```

- 返回值范围: [-1, 1]
- 1表示完全相同
- 0表示无关
- -1表示完全相反

### 内存管理

- **PSRAM**: 存储MobileNetV2模型参数和中间结果
- **Internal RAM**: 控制结构和少量临时数据
- **动态分配**: RGB转换缓冲区使用后及时释放
- **静态分配**: 特征向量等大数组使用全局变量避免栈溢出

### 看门狗处理

- 长耗时推理任务中定期调用`esp_task_wdt_reset()`
- UART任务栈大小设置为8192字节
- 防止系统误判任务卡死而重启

## 未来改进方向

### 短期改进

1. **多模型支持**:
   - 添加人脸识别专用模型
   - 支持物体检测模型（YOLO）
   - 用户可动态切换模型

2. **特征数据库**:
   - 支持存储多个特征向量
   - 实现1:N识别
   - 持久化存储到Flash或SD卡

3. **性能优化**:
   - 实现模型量化优化（INT4）
   - 使用模型剪枝技术
   - 并行推理优化

### 长期规划

1. **云端集成**:
   - 将特征上传到云端
   - 实现大规模人脸识别库
   - 云端模型更新

2. **边缘计算增强**:
   - 多摄像头协同
   - 分布式特征比对
   - 实时视频分析

3. **应用场景扩展**:
   - 智能门禁系统
   - 工业质检
   - 农业监测
   - 安防监控

## 许可证

本项目基于ESP-IDF框架开发，遵循相关开源协议。

## 参考资料

- [ESP-IDF编程指南](https://docs.espressif.com/projects/esp-idf/)
- [esp32-camera驱动](https://github.com/espressif/esp32-camera)
- [esp-who计算机视觉框架](https://github.com/espressif/esp-who)
- [esp-dl深度学习库](https://github.com/espressif/esp-dl)
- [MobileNetV2论文](https://arxiv.org/abs/1801.04381)
- [ESP-DL模型库文档](https://docs.espressif.com/projects/esp-dl/)

## 版本历史

### v2.0 (2026-04-21)
- ✅ 集成MobileNetV2深度学习模型
- ✅ 实现1280维特征向量提取
- ✅ 优化特征比对准确性
- ✅ 添加L2归一化处理
- ✅ 完善错误处理和日志输出

### v1.0 (早期版本)
- ✅ HTTP实时视频流
- ✅ 手工特征提取（HSV+LBP+HOG）
- ✅ 基础相似度比对

## 🔧 编译问题排查

### 问题1: Failed to resolve component 'imagenet_cls'

**错误信息**:
```
CMake Error: Failed to resolve component 'imagenet_cls'.
```

**原因**: ESP-DL组件路径未正确配置

**解决方法**:
1. 确认根目录`CMakeLists.txt`包含`EXTRA_COMPONENT_DIRS`配置
2. 运行环境准备脚本: `setup_env.bat` (Windows)
3. 清理并重新编译:
   ```bash
   idf.py fullclean
   idf.py build
   ```

### 问题2: Git所有权警告

**错误信息**:
```
fatal: detected dubious ownership in repository at 'xxx'
```

**解决方法**:
- **Windows**: 运行 `setup_env.bat` 自动修复
- **手动修复**: 
  ```bash
  git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.1.2
  ```
- **或者忽略**: 这只是警告，不影响编译

### 问题3: Component directory does not contain CMakeLists.txt

**错误信息**:
```
Component directory xxx does not contain a CMakeLists.txt file
```

**原因**: 组件路径指向了错误的目录

**解决方法**:
检查根目录`CMakeLists.txt`中的路径是否正确：
```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/esp-dl"  # 注意是esp-dl/esp-dl
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/models/imagenet_cls"
)
```

### 问题4: PSRAM未启用

**症状**: 运行时内存不足或模型加载失败

**解决方法**:
```bash
idf.py menuconfig
→ Component config → ESP PSRAM → Enable SPI RAM
```

或在`sdkconfig`中确认：
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y
```

### 问题5: 头文件找不到（IDE错误）

**症状**: VSCode显示"无法打开源文件"但实际可以编译

**解决方法**:
```bash
# 重新生成IDE配置
idf.py reconfigure
```

这会自动更新`compile_commands.json`，IDE索引会随之更新。

### 通用故障排除步骤

1. **清理构建**:
   ```bash
   idf.py fullclean
   ```

2. **重新配置**:
   ```bash
   idf.py reconfigure
   ```

3. **详细编译输出**:
   ```bash
   idf.py build -v
   ```

4. **检查依赖**:
   ```bash
   idf.py inspect
   ```

5. **查看日志**:
   ```bash
   cat build/CMakeFiles/CMakeOutput.log
   cat build/CMakeFiles/CMakeError.log
   ```
