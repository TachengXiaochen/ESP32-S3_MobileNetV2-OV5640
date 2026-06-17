# ESP32-S3 CAM AI 开发规范

> **项目名称**: ESP32-S3 MobileNetV2-OV5640 智能资产管理系统  
> **版本**: v3.6-dev (2026-06-17，实时更新)  
> **芯片平台**: ESP32-S3 (双核240MHz, 带PSRAM)  
> **ESP-IDF版本**: >= 5.3.0  
> **主要功能**: 基于MobileNetV2的三视图资产识别与盘点系统  

---

## 📖 项目定位

ESP32-S3作为**智能仓储系统的视觉识别终端**，负责：

1. **图像采集** - OV5640摄像头三视图（正面/侧面/顶部）拍摄
2. **AI推理** - MobileNetV2深度学习模型特征提取（GAP 1280维语义特征）
3. **资产管理** - TF卡存储资产记录、图片、特征向量
4. **多协议通信** - UART0 CLI文本 + UART1 WS63 JSON + WiFi Web调试
5. **云端上报** - L610 4G模块MQTT ThingsKit集成（可选）

### 系统拓扑图

```
                    ┌─────────────┐
                    │  ThingsKit  │
                    │  MQTT 云平台 │
                    └──────┬──────┘
                           │ 4G/LTE
                    ┌──────┴──────┐
                    │   L610 4G   │
                    └──────┬──────┘
                           │ UART2
                    ┌──────┴──────┐
                    │  ESP32-S3   │
                    │ (视觉终端)   │
                    ├──────┬──────┤
               UART0│      │UART1│
                    │      │     │
            ┌───────┴──┐ ┌─┴─────┴──┐
            │ USB转TTL │ │  WS63网关 │
            │(本地调试) │ │(SLE桥接)  │
            └──────────┘ └──────────┘
                    │
            ┌───────┴─────┐
            │  OV5640摄像头│
            │  (DVP接口)    │
            └─────────────┘
                    │
            ┌───────┴─────┐
            │  MicroSD卡   │
            │ (FAT32存储)  │
            └─────────────┘
```

---

## 🏗️ 项目架构

### 模块化设计

```
main/
├── main.c                        # 应用主入口（任务调度、状态机、队列管理）
├── main.h                        # 全局定义、枚举、数据结构
├── CMakeLists.txt                # ESP-IDF构建配置
├── modules/                      # 核心组件模块（5个独立模块）
│   ├── camera/                  # 摄像头驱动层
│   │   ├── camera_module.c/.h   # OV5640初始化、拍摄、JPEG捕获
│   │   └── pin_config.h         # DVP引脚配置（GPIO39/38/40等）
│   ├── ai/                      # AI推理引擎层
│   │   ├── ai_module.c/.h       # AI模块总控（初始化、反初始化）
│   │   ├── mobilenet_wrapper.cpp # MobileNetV2封装（C++调用esp-dl）
│   │   ├── feature_processor.c/.h # GAP特征提取（1280维）
│   │   ├── similarity_matcher.c/.h # 相似度计算（纯余弦+阈值0.90）
│   │   ├── blur_detection.c/.h  # 模糊度检测（拉普拉斯方差）
│   │   └── imagenet_classes.h   # ImageNet分类标签（调试用）
│   ├── system/                  # 系统管理层
│   │   ├── storage/             # 存储子系统
│   │   │   ├── storage_module.c/.h # TF卡初始化、文件系统挂载
│   │   │   └── asset_manager.c/.h  # 资产CRUD操作（注册/盘点/删除）
│   │   ├── executor/            # 业务执行器 ⭐v3.4
│   │   │   ├── business_executor.c/.h # 统一命令处理+双通道输出
│   │   ├── comm/                # 通信接口层 ⭐v3.4拆分
│   │   │   ├── uart_handler_0.c/.h  # UART0 CLI文本处理器
│   │   │   └── uart_handler_1.c/.h  # UART1 WS63 JSON处理器
│   │   ├── verify/              # 验证模块 ⭐v3.2
│   │   │   ├── tag_id_validator.c/.h # Tag ID格式校验
│   │   │   ├── verify_handler.c/.h   # 身份验证逻辑
│   │   │   └── verify_config.h       # 验证配置（阈值、重试次数）
│   │   ├── led/                 # LED指示层
│   │   │   ├── led_indicator.c/.h # WS2812 RGB状态指示
│   │   └── abolished/           # 废弃代码归档（历史版本）
│   ├── 4g/                      # 4G通信层（v3.1）
│   │   ├── l610_driver.c/.h     # L610 AT指令驱动
│   │   ├── l610_mqtt.c/.h       # MQTT客户端封装
│   │   └── l610_manager.c/.h    # 4G模块管理器（心跳、重连）
│   └── web/                     # Web调试层（v3.5）
│       ├── web_server.c/.h      # HTTP服务器（WiFi SoftAP）
│       ├── mjpeg_handler.c/.h   # MJPEG视频流处理
│       └── api_handlers.c/.h    # RESTful API端点
└── docs/                         # 技术文档库
    ├── QUICKSTART.md            # 快速开始指南
    ├── USER_GUIDE.md            # 用户使用手册
    ├── PROTOCOL/                # 协议规范
    │   ├── ESP32_WS63_PROTOCOL.md # WS63通信协议（v3.4）
    │   └── WS63_MONITOR_PROTOCOL.md # 串口屏协议（v2.4）
    └── archive/                 # 历史变更记录
        ├── CHANGELOG_v3.5.md    # v3.5变更日志
        └── ...                  # 其他版本报告
```

### 架构铁律（不可违反）

#### 1. 模块解耦原则

**分层架构**：
```
┌─────────────────────────────────────┐
│      Application Layer (main.c)     │
│      - 状态机管理                     │
│      - 任务调度                       │
│      - 队列分发                       │
└──────────┬──────────┬───────────────┘
           │          │
    ┌──────┴───┐  ┌──┴────────┐
    │ Business │  │   Web     │
    │ Executor │  │  Server   │
    └──────┬───┘  └───────────┘
           │
    ┌──────┴──────────────────┐
    │   Communication Layer   │
    ├──────┬──────┬───────────┤
    │UART0 │ UART1│  L610     │
    │ CLI  │ WS63 │  MQTT     │
    └──────┴──────┴───────────┘
           │
    ┌──────┴──────────────────┐
    │   Hardware Abstraction  │
    ├──────┬──────┬───────────┤
    │Camera│  AI  │ Storage   │
    │OV5640│MV2   │ FATFS     │
    └──────┴──────┴───────────┘
```

**规则**：
- ✅ 上层可以调用下层接口
- ❌ 同层模块禁止直接调用（必须通过main.c或business_executor中转）
- ❌ 下层禁止反向依赖上层
- ✅ 所有跨模块通信通过FreeRTOS队列或信号量

#### 2. 命名规范

**文件命名**：
- 模块文件：`{module_name}.c/.h`（如`camera_module.c`）
- 子模块目录：小写+下划线（如`storage/`、`executor/`）

**函数命名**：
- 公共API：`{module}_{action}()`（如`camera_module_init()`）
- 内部函数：`_{module}_{action}()`（如`_parse_json_command()`）
- 回调函数：`_{event}_callback()`（如`_capture_complete_callback()`）

**变量命名**：
- 全局变量：`g_{description}`（如`g_camera_ready`）
- 静态全局：`s_{description}`（如`s_uart_queue`）
- 局部变量：小写+下划线（如`feature_vector`）
- 宏定义：大写+下划线（如`FEATURE_VEC_SIZE`）

**类型定义**：
- 枚举：`{module}_state_t`（如`camera_state_t`）
- 结构体：`{module}_config_t`（如`verify_context_t`）
- 指针类型：`{type}_t *`（保持一致性）

#### 3. 错误处理规范

**返回值约定**：
```c
// ✅ 正确：使用bool表示成功/失败
bool camera_module_init(void);

// ✅ 正确：使用esp_err_t表示详细错误码
esp_err_t asset_manager_save(const asset_record_t *record);

// ❌ 错误：混用int和bool
int init_camera();  // 不明确
```

**错误日志分级**：
```c
ESP_LOGI(TAG, "正常信息");      // INFO: 关键流程节点
ESP_LOGW(TAG, "警告信息");      // WARNING: 可恢复异常
ESP_LOGE(TAG, "错误信息");      // ERROR: 严重错误
ESP_LOGD(TAG, "调试信息");      // DEBUG: 详细调试（发布时关闭）
```

**看门狗保护**：
```c
// 长耗时操作前后必须复位看门狗
SAFE_WDT_RESET();  // 宏定义：esp_task_wdt_reset()
ai_module_inference();  // 可能耗时>1s
SAFE_WDT_RESET();
```

#### 4. 内存管理规范

**PSRAM使用**：
- ✅ MobileNetV2模型权重加载到PSRAM
- ✅ 特征向量缓冲区使用PSRAM（1280维×4字节=5KB）
- ❌ 避免在PSRAM上分配小对象（<1KB）

**动态内存**：
```c
// ✅ 正确：检查分配结果
uint8_t *jpeg_buf = malloc(jpeg_size);
if (jpeg_buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
    return false;
}

// ✅ 正确：及时释放
free(jpeg_buf);
jpeg_buf = NULL;  // 防止悬空指针

// ❌ 错误：未检查NULL
uint8_t *buf = malloc(size);
memcpy(buf, src, size);  // 可能崩溃
```

**栈空间限制**：
- FreeRTOS任务栈：最小4KB（推荐8KB用于AI任务）
- 局部数组：避免>1KB的栈分配（改用malloc或static）

---

## 🔧 核心功能模块

### 1. 摄像头驱动层 (camera)

**职责**：OV5640硬件抽象、图像采集、JPEG编码

**关键API**：
```c
bool camera_module_init(void);                          // 初始化
bool camera_module_capture_jpeg(uint8_t **buf, size_t *len); // 捕获JPEG
bool camera_module_capture_and_process(float *feat, int size, float *blur); // 捕获+特征
void camera_module_deinit(void);                        // 反初始化
```

**硬件配置**：
- DVP接口：GPIO39(XCLK), GPIO38(SIOD), GPIO40(SIOC), GPIO47(D7-D0)
- XCLK频率：20MHz
- 分辨率：QVGA (320×240) 用于推理，UXGA (1600×1200) 用于保存
- 像素格式：JPEG（硬件编码）

**互斥锁机制**：
```c
// 摄像头资源由xCameraMutex保护
xSemaphoreTake(xCameraMutex, portMAX_DELAY);
camera_module_capture_jpeg(&buf, &len);
xSemaphoreGive(xCameraMutex);
```

**注意事项**：
- ⚠️ GPIO17/18已迁移至GPIO47/21（避免与DVP冲突）
- ⚠️ 反初始化必须在摄像头空闲时执行
- ⚠️ JPEG缓冲区由调用者负责释放

---

### 2. AI推理引擎层 (ai)

**职责**：MobileNetV2模型加载、特征提取、相似度计算

**关键特性** ⭐v3.5：
- ✅ **GAP 1280维特征**：替代1000维logits，语义表达能力更强
- ✅ **纯余弦相似度**：移除无效Euclidean混合，阈值统一0.90
- ✅ **自适应帧数**：默认1帧，边缘情况补充至3帧（blur_score ∈ [12.5, 37.5]）
- ✅ **模糊度检测**：拉普拉斯方差算法，降采样2×（160×120），速度提升4倍

**关键API**：
```c
bool ai_module_init(void);                              // 初始化模型
void ai_module_deinit(void);                            // 释放模型
float ai_module_calculate_confidence(const float *f1, const float *f2, int size); // 余弦相似度
bool ai_module_match_features(const float *f1, const float *f2, int size, 
                              asset_class_t cls, similarity_result_t *result); // 完整匹配
```

**性能指标**：
| 指标 | v3.4 | v3.5 | 说明 |
|------|------|------|------|
| 单次推理时间 | ~1.2s | **~0.9s** | 优化预处理 |
| 特征维度 | 1000 logits | **1280 GAP** | 语义特征 |
| 模糊检测耗时 | ~200ms | **~50ms** | 降采样提速 |
| 三视图总耗时 | 30-60s | **~8.5s** | 降低80-85% |

**模型文件**：
- 路径：`/spiffs/mobilenet_v2.espdl`
- 大小：~4MB（量化INT8版本）
- 输入：320×240 RGB图像
- 输出：1280维GAP特征向量

**注意事项**：
- ⚠️ 模型加载耗时~3s，仅在启动时执行一次
- ⚠️ 推理期间持有xCameraMutex，阻塞其他摄像头操作
- ⚠️ PSRAM不足会导致模型加载失败

---

### 3. 系统管理层 (system)

#### 3.1 存储子系统 (storage)

**职责**：TF卡初始化、FATFS挂载、资产CRUD操作

**目录结构**：
```
/sdcard/
└── assets/
    ├── 0x0001.dat              # 资产记录（15KB）
    ├── 0x0001_front.jpg        # 正面图片（10-30KB）
    ├── 0x0001_side.jpg         # 侧面图片
    ├── 0x0001_top.jpg          # 顶部图片
    └── ...
```

**关键API**：
```c
bool storage_module_init(void);                             // 初始化TF卡
bool asset_manager_save(const asset_record_t *record);      // 保存资产
bool asset_manager_load(const char *tag_id, asset_record_t *record); // 加载资产
bool asset_manager_delete(const char *tag_id);              // 删除资产
esp_err_t asset_manager_list(asset_list_t *list);           // 列出所有资产
```

**资产记录结构**：
```c
typedef struct {
    char tag_id[TAG_ID_STR_LEN];        // Tag ID字符串（"0x0001"）
    char item_name[128];                // 物品名称
    char storage_area;                  // 存放区域（A-Z）
    uint32_t quantity;                  // 库存数量
    float front_feature[FEATURE_VEC_SIZE]; // 正面特征（1280维）
    float side_feature[FEATURE_VEC_SIZE];  // 侧面特征
    float top_feature[FEATURE_VEC_SIZE];   // 顶部特征
    bool is_valid;                      // 有效性标志
} asset_record_t;
```

**注意事项**：
- ⚠️ TF卡必须为FAT32格式
- ⚠️ 写入操作后调用`fflush()`确保数据落盘
- ⚠️ 删除操作不可恢复，需二次确认

#### 3.2 业务执行器 (executor) ⭐v3.4

**职责**：统一命令处理引擎、双通道输出（CLI文本/JSON协议）

**架构优势**：
- ✅ **解耦通信层**：business_executor不关心数据来源（UART0/UART1/MQTT）
- ✅ **统一状态机**：IDLE → HARDWARE_INIT → WAITING_CAPTURE → CAPTURING → FINALIZING
- ✅ **事件驱动**：通过回调机制通知上层模块

**关键API**：
```c
esp_err_t business_executor_init(void);                     // 初始化
esp_err_t business_execute(be_cmd_t cmd, const char *json); // 执行命令
void business_register_callback(be_event_t evt, be_event_cb_t cb); // 注册回调
be_state_t business_get_state(void);                        // 获取当前状态
```

**支持命令**：
| 命令 | 说明 | 示例 |
|------|------|------|
| `BE_CMD_REGISTER` | 资产注册 | `{"cmd":"register","tag_id":"0x0001","quantity":50}` |
| `BE_CMD_INVENTORY` | 资产盘点 | `{"cmd":"inventory","tag_id":"0x0001"}` |
| `BE_CMD_OUTBOUND` | 出库核验 | `{"cmd":"outbound","tag_id":"0x0001","remove_qty":5}` |
| `BE_CMD_CAPTURE_FRONT` | 拍摄正面 | `{"cmd":"capture","view":"front"}` |
| `BE_CMD_DELETE` | 删除资产 | `{"cmd":"delete","tag_id":"0x0001"}` |

**事件类型**：
- `BE_EVT_HARDWARE_READY` - 硬件初始化完成
- `BE_EVT_CAPTURE_PROGRESS` - 单视图拍摄完成
- `BE_EVT_TASK_DONE` - 任务完成
- `BE_EVT_ERROR` - 错误报告

**注意事项**：
- ⚠️ 同一时间只能执行一个任务（状态机互斥）
- ⚠️ 取消命令（`BE_CMD_CANCEL`）可在任意状态中断任务
- ⚠️ 回调函数中禁止执行耗时操作

#### 3.3 通信接口层 (comm) ⭐v3.4拆分

**职责**：UART0 CLI文本解析 + UART1 WS63 JSON协议处理

**模块拆分**：
- `uart_handler_0.c` - CLI文本命令解析（人读文本）
- `uart_handler_1.c` - WS63 JSON协议解析（机器可读）

**硬件连接**：
```
UART0 (CLI): GPIO43(TX)/GPIO44(RX) ↔ USB转TTL（本地调试）
UART1 (WS63): GPIO47(TX)/GPIO21(RX) ↔ WS63网关（JSON协议）
```

**关键特性**：
- ✅ **异步非阻塞**：UART接收在独立FreeRTOS任务中运行
- ✅ **优先级相同**：两个UART任务优先级均为5
- ✅ **共享business_executor**：解析后的命令统一交给业务执行器

**注意事项**：
- ⚠️ UART1已迁移至GPIO47/21（避免与摄像头DVP冲突）
- ⚠️ JSON缓冲区动态分配，使用后及时释放
- ⚠️ CLI模式保留用于调试，不影响WS63协议

#### 3.4 验证模块 (verify) ⭐v3.2

**职责**：Tag ID格式校验、身份验证逻辑

**关键API**：
```c
bool tag_id_validate(const char *tag_id);                   // 验证Tag ID格式
esp_err_t verify_handler_start(verify_context_t *ctx);      // 启动验证
esp_err_t verify_handler_check(const float *current, const float *stored); // 比对特征
```

**验证流程**：
```
1. 用户输入Tag ID → tag_id_validate() 检查格式（0x0001-0xFFFF）
2. 查询asset_manager_load() 获取已存特征
3. 拍摄正视图 → 提取特征向量
4. verify_handler_check() 比对余弦相似度
5. 相似度≥0.90 → 验证通过，允许累加数量
6. 相似度<0.90 → 验证失败，拒绝操作
```

**配置参数**（verify_config.h）：
```c
#define VERIFY_THRESHOLD      0.90f    // 验证阈值
#define VERIFY_MAX_RETRIES    3        // 最大重试次数
#define VERIFY_FEATURE_DIM    1280     // 特征维度
```

#### 3.5 LED指示层 (led)

**职责**：WS2812 RGB LED状态反馈

**颜色含义**：
| LED状态 | 颜色 | 含义 |
|---------|------|------|
| 红色常亮 | 🔴 | 待机/摄像头关闭 |
| 绿色常亮 | 🟢 | 注册模式 |
| 蓝色常亮 | 🔵 | 盘点模式 |
| 绿色闪烁 | 🟢 | 注册拍摄中 |
| 蓝色闪烁 | 🔵 | 盘点拍摄中 |

**硬件配置**：
- 数据引脚：GPIO48
- 供电要求：5V外部电源（3.3V驱动能力不足）
- 亮度：默认50%（128/255）

---

### 4. 4G通信层 (4g) ⭐v3.1

**职责**：L610模块驱动、MQTT客户端、云端上报

**关键API**：
```c
bool l610_manager_init(void);                             // 初始化4G模块
bool l610_mqtt_connect(const char *host, int port);       // 连接MQTT
bool l610_mqtt_publish(const char *topic, const char *payload); // 发布消息
void l610_manager_heartbeat(void);                        // 心跳保活
```

**ThingsKit配置**：
- 服务器：`mqtt.thingskit.com`
- 端口：1883 (TCP) / 8883 (TLS)
- QoS等级：1 (至少一次送达)
- ClientID格式：`ESP32-{MAC地址}`

**主动上报机制**：
- MQTT意外断开 → `l610_error`
- 模块失联 → `L610_NOT_RESPONDING`
- 网络异常 → `NETWORK_DETACHED`

**硬件连接**：
```
ESP32-S3                L610 Module
├── GPIO4 (TX) ──────► RX
├── GPIO5 (RX) ◄────── TX
└── GND ────────────── GND
```

**注意事项**：
- ⚠️ L610峰值电流可达2A，建议使用独立电源
- ⚠️ AT指令响应超时设置为5s
- ⚠️ MQTT重连间隔指数退避（1s → 2s → 4s → 8s → 16s）

---

### 5. Web调试层 (web) ⭐v3.5

**职责**：WiFi SoftAP、MJPEG实时预览、RESTful API

**重要说明**：Web实时流仅用于调试和拍摄辅助，实际演示不需要。赛题要求WS63做AP/Host。

**HTTP API端点**：
| 方法 | 端点 | Content-Type | 说明 |
|------|------|-------------|------|
| GET | `/` | text/html | 主页（SPIFFS） |
| GET | `/stream` | multipart/x-mixed-replace | MJPEG实时视频流 |
| GET | `/api/status` | application/json | 系统运行状态 |
| GET | `/api/snapshot` | image/jpeg | 单帧JPEG快照 |
| GET | `/api/frames?tag_id=X` | application/json | 指定资产三视图信息 |
| GET | `/api/image?tag_id=X&view=Y` | image/jpeg | 从SD卡提供JPEG文件 |
| GET | `/api/assets` | application/json | 已注册资产列表 |

**技术实现**：
- **WiFi模式**: SoftAP, SSID: `ESP32-CAM-AI`, IP: 192.168.4.1
- **互斥锁机制**: AI管道优先级7 > HTTP服务器优先级5，100ms超时保证AI总能抢占摄像头
- **时序分离**: WiFi在SD卡、模型、摄像头全部初始化完毕3秒后才启动，避免PSRAM DMA竞争
- **分区调整**: factory 6M→7M（固件膨胀70KB），storage 2M→1M（前端仅~10KB）

**注意事项**：
- ⚠️ Web服务器与AI推理共享摄像头资源，通过xCameraMutex互斥
- ⚠️ MJPEG流帧率8-12fps，受推理任务影响会波动
- ⚠️ 生产环境建议禁用Web服务器以节省资源

---

## 📋 开发规范

### 代码风格

#### 1. 头文件保护

```c
#ifndef CAMERA_MODULE_H
#define CAMERA_MODULE_H

// ... 内容 ...

#endif // CAMERA_MODULE_H
```

#### 2. 函数注释（Doxygen风格）

```c
/**
 * @brief 初始化摄像头硬件
 * @return true 成功, false 失败
 * @note 必须在调用其他摄像头API之前执行
 * @warning 初始化失败可能导致后续操作崩溃
 */
bool camera_module_init(void);
```

#### 3. 条件编译

```c
#ifdef CONFIG_CAMERA_DEBUG
    ESP_LOGD(TAG, "Debug: Camera register value = 0x%02X", reg_val);
#endif
```

#### 4. 常量定义

```c
// ✅ 正确：使用const或enum
static const int FEATURE_VEC_SIZE = 1280;
typedef enum { STATE_IDLE, STATE_BUSY } state_t;

// ❌ 错误：使用魔法数字
int size = 1280;  // 这是什么？
```

### 模块解耦原则

**依赖方向**：
```
main.c (应用层)
  ↓
business_executor (业务层)
  ↓
uart_handler_0/1 (通信层)
  ↓
camera/ai/storage (硬件抽象层)
```

**禁止行为**：
- ❌ `camera_module.c` 直接调用 `ai_module_init()`
- ❌ `uart_handler_1.c` 直接访问 `g_camera_state`
- ❌ `asset_manager.c` 直接操作UART发送数据

**正确做法**：
- ✅ 通过`main.c`的任务队列传递消息
- ✅ 通过`business_executor`的事件回调通知上层
- ✅ 通过全局变量（`g_xxx`）共享只读状态

### 重构工作流（v3.6 确立）

#### UART0/UART1 双通道设计原则

- **UART0**：纯调试用，CLI 交互式，15 状态机 + 部分绕过 `be_execute()` 均可接受。不需要生产级质量。
- **UART1**：生产路径，JSON 协议，完整走 `be_execute()` 事件驱动。是质量的唯一标准。
- **两者不同时使用**，不存在并发竞争。

#### main.c 职责边界

```
main.c（~100行）：仅负责
  1. 全局变量定义（app_context_t + IPC + 特征缓冲区）
  2. 模块初始化编排（app_main）
  3. FreeRTOS 任务创建

命令处理    → app_handlers.c（camera_ai_task + 8 handler + be_output_callback）
推理任务    → modules/ai/inference_task.c
存储任务    → modules/system/storage/storage_task.c
UART0 初始化 → modules/system/comm/uart_handler_0.c
```

#### 全局变量管理：app_context_t 模式

**问题**：31 个独立全局变量散布在 ~10 个文件中，通过函数体内 `extern` 隐式耦合。

**方案**：
```c
// main.h — 定义上下文结构体 + 兼容宏
typedef struct {
    bool camera_ready;           // 模块状态
    bool storage_ready;
    char current_tag_id[7];      // 当前任务
    // ... 共 20 个字段
} app_context_t;

extern app_context_t g_ctx;

// 向后兼容宏（其他文件无需立即修改）
#define g_camera_ready   (g_ctx.camera_ready)
#define g_current_tag_id (g_ctx.current_tag_id)
// ...

// 保持独立的部分（不在 g_ctx 中）
extern QueueHandle_t xSystemQueue;   // IPC 句柄（FreeRTOS 惯用法）
extern float g_front_feature[];      // 特征缓冲区（PSRAM 对齐）
```

**迁移策略**：
1. 定义结构体 + 兼容宏（main.h）→ 零行为变更
2. 其他文件通过宏透明访问 → 无需修改
3. 后续迭代逐步改为 `g_ctx.field` 直接访问 → 移除宏

#### 低风险重构三步骤

| 步骤 | 内容 | 风险 | 验证 |
|------|------|------|------|
| 1. 文件拆分 | 将函数体移到新文件，不改逻辑 | **零** | 每个函数移动后 `idf.py build` |
| 2. 消除冗余 extern | 删除 `main.h` 已声明的函数体内 `extern` | **零** | 编译器验证 |
| 3. 变量分组 | 定义结构体 + 兼容宏 | **低** | 宏保证兼容，编译即验证 |

**核心原则**：
- ✅ 每步独立可验证（编译 + UART0 CLI 冒烟测试）
- ✅ 不改业务逻辑，只做文件组织和变量分组
- ✅ 兼容宏保证其他文件无需立即修改
- ❌ 不在重构中同时改状态机或业务逻辑

#### 常见反模式（本次修复）

| 反模式 | 表现 | 修复 |
|--------|------|------|
| 函数体内 `extern` | `business_executor.c` 中 23 处散落 extern | 删除（main.h 已声明），或集中到文件顶部 |
| 跨文件 `sizeof` 不完整类型 | `sizeof(g_front_feature)` 在分离的 .c 中失败 | 改用 `FEATURE_VEC_SIZE * sizeof(float)` |
| `extern` 与宏冲突 | `extern char g_l610_client_id[];` 被宏展开为非法语法 | 删除 extern，宏自动提供访问 |
| CLI 绕过业务层 | `uart_handler_0.c` 直接写 `g_be_state` / `g_be_task` | 调试路径可接受，生产路径已修复 |

---

### 错误处理策略

**三级错误处理**：
```c
// Level 1: 可恢复错误（重试）
if (!camera_module_capture_jpeg(&buf, &len)) {
    ESP_LOGW(TAG, "Capture failed, retrying...");
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!camera_module_capture_jpeg(&buf, &len)) {
        ESP_LOGE(TAG, "Capture failed after retry");
        return false;
    }
}

// Level 2: 不可恢复错误（返回）
if (!storage_module_init()) {
    ESP_LOGE(TAG, "Storage init failed, aborting");
    return ESP_FAIL;
}

// Level 3: 致命错误（重启）
if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1024 * 1024) {
    ESP_LOGE(TAG, "PSRAM critically low, restarting");
    esp_restart();
}
```

### 性能优化建议

#### 1. 减少内存拷贝

```c
// ✅ 正确：直接使用摄像头缓冲区
uint8_t *jpeg_buf;
size_t jpeg_len;
camera_module_capture_jpeg(&jpeg_buf, &jpeg_len);
storage_write("/sdcard/test.jpg", jpeg_buf, jpeg_len);

// ❌ 错误：不必要的拷贝
uint8_t temp_buf[JPEG_MAX_SIZE];
camera_module_capture_jpeg(&jpeg_buf, &jpeg_len);
memcpy(temp_buf, jpeg_buf, jpeg_len);  // 浪费时间和内存
storage_write("/sdcard/test.jpg", temp_buf, jpeg_len);
```

#### 2. 批量操作

```c
// ✅ 正确：一次性保存三视图
asset_manager_save_triple(front_feat, side_feat, top_feat);

// ❌ 错误：三次独立保存
asset_manager_save_feature("front", front_feat);
asset_manager_save_feature("side", side_feat);
asset_manager_save_feature("top", top_feat);
```

#### 3. 延迟初始化

```c
// ✅ 正确：按需初始化AI模型
if (need_inference) {
    ai_module_init();  // 仅在需要时加载
    // ... 推理 ...
    ai_module_deinit(); // 完成后释放
}

// ❌ 错误：启动时立即加载所有模块
app_main() {
    ai_module_init();  // 占用4MB PSRAM，即使用户不使用AI功能
}
```

---

## 🚀 快速开始

### 环境准备

**必需工具**：
- ESP-IDF >= 5.3.0
- CMake 3.10+
- Python 3.8+
- VSCode + ESP-IDF插件（推荐）

**硬件要求**：
- ESP32-S3开发板（带PSRAM，推荐8MB）
- OV5640摄像头模块（DVP接口）
- MicroSD/TF卡（FAT32格式，≥8GB）
- USB数据线
- WS2812 RGB LED（可选，GPIO48）
- L610 4G模块（可选，v3.1）

### 编译步骤

```bash
# 1. 设置目标芯片
idf.py set-target esp32s3

# 2. 清理构建（重要！）
idf.py fullclean

# 3. 配置项目（可选）
idf.py menuconfig

# 4. 编译
idf.py build

# 5. 烧录并监控
idf.py flash monitor -p COM3
```

### 首次运行

1. **硬件连接**：
   - OV5640摄像头 → ESP32-S3 DVP接口
   - MicroSD卡 → 插入卡槽
   - USB转TTL → UART0 (GPIO43/44)
   - WS2812 LED → GPIO48（可选）

2. **上电启动**：
   ```
   I (0) cpu_start: Starting scheduler on both cores
   I (100) camera_ai: UART initialized at 115200 baud
   I (200) camera_ai: Initializing storage module...
   I (300) storage: SD card mounted successfully
   I (400) camera_ai: Initializing camera module...
   I (500) camera: Camera PID = 0x5640
   I (600) camera_ai: Camera initialized successfully
   I (700) camera_ai: Initializing AI module...
   I (3800) ai: MobileNetV2 model loaded (4.2MB)
   I (3900) camera_ai: AI module initialized
   I (3950) camera_ai: System ready! Waiting for Tag ID...
   ```

3. **验证通信**：
   ```bash
   # 输入Tag ID
   0x0001
   
   # 预期响应
   ========== MAIN MENU ==========
     r - Register new asset
     c - Inventory existing asset
     d - Delete asset
     l - List all assets
     i - System information
   =================================
   [GUIDE] Please select an option: 
   ```

---

## 📊 性能指标

| 指标 | v3.4 | v3.5 | 说明 |
|------|------|------|------|
| **三视图推理时间** | 30-60s | **~8.5s** | ⭐降低80-85% |
| **每视图帧数** | 3帧固定 | 1帧（边缘3帧） | ⭐自适应策略 |
| **特征维度** | 1000 logits | **1280 GAP** | ⭐语义特征 |
| **同物品cosine** | ~1.0 (bug) | **>0.85** | ⭐修复覆盖bug |
| **异物品cosine** | ~1.0 (bug) | **0.55-0.65** | ⭐区分度提升 |
| **匹配算法** | 70%cosine+30%euclidean | **100% cosine** | ⭐简化有效 |
| **匹配阈值** | 0.70-0.85 | **0.90** | ⭐统一提升 |
| **单次拍摄时间** | ~800ms | ~600ms | 模糊检测降采样 |
| **特征提取时间** | ~1.2s | ~0.9s | 移除冗余计算 |
| **相似度计算** | <10ms | <10ms | 纯余弦更高效 |
| **出库完整流程** | ❌ 不支持 | **~7.5秒** | ⭐新功能 |
| **拍摄反馈延迟** | ~7.5秒 | **~200ms** | ⭐37倍提升 |
| **内存占用** | ~4MB | ~4MB | PSRAM使用 |
| **TF卡写入速度** | ~500KB/s | ~500KB/s | 取决于TF卡等级 |

---

## 🐛 常见问题

### Q1: 编译报错"CONFIG_LOG_MAXIMUM_LEVEL未定义"？

**原因**：IDE静态分析误报，该宏由CMake在编译时生成

**解决**：
- ✅ 忽略IDE警告，以真实编译为准
- ✅ 执行`idf.py build`验证实际状态
- ❌ 不要手动添加`#define CONFIG_LOG_MAXIMUM_LEVEL`

### Q2: PSRAM初始化失败？

**排查步骤**：
1. 检查menuconfig中PSRAM是否启用（Component config → ESP32S3-specific → SPI RAM config）
2. 确认PSRAM频率设置为40MHz（过高会导致不稳定）
3. 查看启动日志中的PSRAM检测结果

**解决方案**：
```
idf.py menuconfig
→ Component config
  → ESP32S3-specific
    → SPI RAM config
      → Mode: Quad PSRAM
      → Frequency: 40MHz
```

### Q3: 摄像头初始化失败（Camera PID = 0x0000）？

**原因**：DVP引脚连接错误或XCLK频率不匹配

**排查步骤**：
1. 检查摄像头排线连接（特别是XCLK、SIOD、SIOC）
2. 确认pin_config.h中的引脚定义与硬件一致
3. 尝试降低XCLK频率（20MHz → 10MHz）

**诊断代码**：
```c
// 在camera_module_init()中添加调试日志
ESP_LOGI(TAG, "XCLK freq = %d Hz", config.xclk_freq_hz);
ESP_LOGI(TAG, "SIOD pin = %d, SIOC pin = %d", config.sccb_sda_io_num, config.sccb_scl_io_num);
```

### Q4: TF卡挂载失败？

**常见错误码**：
- `ESP_ERR_INVALID_STATE` - TF卡未插入或接触不良
- `ESP_ERR_NOT_SUPPORTED` - 文件系统格式错误（非FAT32）
- `ESP_ERR_NO_MEM` - 堆内存不足

**解决步骤**：
1. 重新插拔TF卡，确认卡槽接触良好
2. 在电脑上格式化为FAT32（分配单元大小4096字节）
3. 检查menuconfig中SDMMC配置是否正确
4. 查看启动日志中的具体错误信息

### Q5: AI推理耗时过长（>5s/次）？

**优化建议**：
1. 确认模型文件位于`/spiffs/mobilenet_v2.espdl`
2. 检查PSRAM剩余空间（应>2MB）
3. 降低输入分辨率（UXGA → QVGA）
4. 启用模型量化（INT8而非FP32）

**性能监控**：
```c
int64_t start = esp_timer_get_time();
ai_module_inference(image);
int64_t elapsed = (esp_timer_get_time() - start) / 1000;
ESP_LOGI(TAG, "Inference time: %lld ms", elapsed);
```

### Q6: WebSocket连接后立即断开？

**原因**：Web服务器与AI推理争夺摄像头资源

**解决方案**：
- ✅ 等待AI推理完成后再访问Web页面
- ✅ 降低MJPEG流帧率（30fps → 10fps）
- ✅ 在生产环境中禁用Web服务器

---

## 📚 相关文档

### 核心文档
- 📘 [README.md](README.md) - 项目总览（v3.5）
- 📗 [docs/QUICKSTART.md](docs/QUICKSTART.md) - 快速开始指南
- 📙 [docs/USER_GUIDE.md](docs/USER_GUIDE.md) - 用户使用手册

### 协议文档
- 📕 [docs/PROTOCOL/ESP32_WS63_PROTOCOL.md](docs/PROTOCOL/ESP32_WS63_PROTOCOL.md) - WS63通信协议（v3.4）
- 📔 [docs/PROTOCOL/WS63_MONITOR_PROTOCOL.md](docs/PROTOCOL/WS63_MONITOR_PROTOCOL.md) - 串口屏协议（v2.4）

### 变更记录
- 📝 [docs/archive/CHANGELOG_v3.5.md](docs/archive/CHANGELOG_v3.5.md) - v3.5变更日志
- 📝 [docs/archive/20260606_INFERENCE_SPEED_OPTIMIZATION.md](docs/archive/20260606_INFERENCE_SPEED_OPTIMIZATION.md) - AI推理优化报告
- 📝 [docs/archive/20260608_GAP_FEATURE_AND_FIXES.md](docs/archive/20260608_GAP_FEATURE_AND_FIXES.md) - GAP特征和匹配修复报告
- 📝 [docs/archive/20260609_WEB_LIVE_PREVIEW.md](docs/archive/20260609_WEB_LIVE_PREVIEW.md) - Web实时预览实施报告
- 📝 [docs/archive/20260617_MAIN_C_REFACTOR.md](docs/archive/20260617_MAIN_C_REFACTOR.md) - main.c去上帝化重构报告

### WS63端文档
- 📖 [ws63/README.md](ws63/README.md) - WS63网关项目说明
- 📖 [ws63/My_project_63/CLAUDE.md](ws63/My_project_63/CLAUDE.md) - WS63开发规范

---

## 📞 技术支持

### 仓库链接
- **项目主页**: [星闪智能盘点系统](https://gitee.com/star-flash-smart-inventory)
- **ESP32-S3端仓库**: 
  - GitHub: [ESP32-S3_MobileNetV2-OV5640](https://github.com/TachengXiaochen/ESP32-S3_MobileNetV2-OV5640)
  - Gitee镜像: [ESP32-S3_MobileNetV2-OV5640](https://gitee.com/star-flash-smart-inventory/ESP32-S3_MobileNetV2-OV5640)
- **WS63端仓库**（主要负责人）: [ws63_bs2x_sle_project](https://gitee.com/star-flash-smart-inventory/ws63_bs2x_sle_project)

### 问题反馈
- **GitHub Issues**: [GitHub Issues](https://github.com/TachengXiaochen/ESP32-S3_MobileNetV2-OV5640/issues)
- **Gitee Issues**: [Gitee Issues](https://gitee.com/star-flash-smart-inventory/ESP32-S3_MobileNetV2-OV5640/issues)
- **邮箱**: 202500201056@stumail.sztu.edu.cn

### 维护者
- **ESP32-S3端**: TcXc
- **WS63端**: Star Flash Smart Inventory Team
- **最后更新**: 2026-06-09

---

## 📄 许可证

本项目采用 MIT 许可证。详见LICENSE文件。

---

**祝你开发顺利！** 🎉
