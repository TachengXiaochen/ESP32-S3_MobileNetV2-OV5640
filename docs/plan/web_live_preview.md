# Web实时预览方案

## 背景

当前调试ESP32-S3 CAM AI v3.4时，无法实时看到摄像头画面，也无法方便查看已保存的FST（Front/Side/Top三视图）帧图片。需要通过ESP32 WiFi自建热点，手机/电脑连接后访问IP即可看到：
1. **实时摄像头画面** (MJPEG流)
2. **ESP32当前工作状态** (堆内存、摄像头就绪、存储就绪、业务执行器状态、当前Tag ID等)
3. **FST三视图帧图片** (SD卡中已保存的 front/side/top JPEG)

## 涉及文件

### 新建文件
| 文件 | 用途 |
|------|------|
| `main/modules/web/web_server.h` | Web模块公共API |
| `main/modules/web/web_server.c` | WiFi SoftAP初始化、SPIFFS挂载、HTTP服务器启动 |
| `main/modules/web/mjpeg_handler.c` | MJPEG流端点 `/stream` |
| `main/modules/web/api_handlers.c` | JSON API端点 (`/api/status`, `/api/frames`, `/api/image`, `/api/assets`, `/api/snapshot`) |
| `spiffs_web/index.html` | 前端单页面 |
| `spiffs_web/style.css` | 样式表 |
| `spiffs_web/app.js` | 前端逻辑 |

### 修改文件
| 文件 | 改动 |
|------|------|
| `main/CMakeLists.txt` | 添加web模块源文件、include目录、新增REQUIRES: `esp_wifi esp_http_server esp_netif lwip spiffs` |
| `main/main.c` | 在`app_main()`末尾添加 `web_server_init()` 调用 |
| 项目根 `CMakeLists.txt` | 添加 `spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_web FLASH_IN_PROJECT)` |

## ⚠️ SD卡+WiFi共存风险与缓解

SDMMC和WiFi同时活动时存在以下冲突点：
- **PSRAM DMA竞争**：SDMMC和WiFi都通过DMA访问PSRAM，同时初始化可能总线冲突
- **堆内存碎片**：WiFi分配大量小缓冲区（RX/TX buffer），SD卡JPEG读写需要连续大块
- **中断延迟**：WiFi中断可能打断SDMMC时序敏感的命令/响应周期
- **栈溢出**：WiFi事件处理 + SD卡文件IO嵌套调用时栈深度叠加

**缓解措施**：
1. **时序分离**：WiFi 在 SD 卡和模型加载完 3 秒后才初始化 ← 唯一有效的措施
2. **错误隔离**：WiFi 或 SPIFFS 失败不影响主业务（AI/摄像头/存储继续工作）

> ⚠️ **教训**：不要覆盖 `WIFI_INIT_CONFIG_DEFAULT()` 的默认值。`cache_tx_buf_num=0` 导致 TX 包静默丢弃（默认 32），是 HTTP 不通的根因。

---

## 实现方案

### 1. 初始化时序（关键）

**WiFi 在 L610 之前启动**，避免被 L610 的 AT 超时（5秒）阻塞。

```c
// main.c app_main() 末尾
// 现有初始化全部完成后（SD卡、模型、摄像头、UART 等）...

// ⚠️ 延迟3秒确保SD卡DMA、PSRAM完全释放后再启动WiFi
vTaskDelay(pdMS_TO_TICKS(3000));
ESP_LOGI(TAG, "Starting web server (delayed init)...");
web_server_init();

// WiFi 就绪后再初始化 L610（不阻塞调试用的Web预览）
ret = l610_manager_init();
if (ret == ESP_OK) { ... }
```

`web_server_init()` 内部进一步拆分步骤，每步之间加短延迟：

```
1. SPIFFS挂载       → vTaskDelay(100ms)
2. WiFi netif创建   → vTaskDelay(200ms)  
3. WiFi init+start  → 等待WIFI_EVENT_AP_START (信号量, 最多10s)
4. HTTP server启动  → vTaskDelay(100ms)
```

### 2. WiFi SoftAP模式

**使用完全默认的 WiFi 配置**，不覆盖任何参数。之前覆盖 `cache_tx_buf_num=0` 是 TCP/HTTP 不通的根因。

```c
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // 不覆盖任何字段
```

SoftAP参数：
```
SSID: "ESP32-CAM-AI"
密码: ""  (先开放模式验证，后续可加 WPA2)
认证: WIFI_AUTH_OPEN
信道: 1
IP: 192.168.4.1
```

### 3. SPIFFS挂载

利用分区表中已有的2MB `storage` 分区：

```c
esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = 5,
    .format_if_mount_failed = false,
};
esp_vfs_spiffs_register(&conf);
```

**注意**：此分区在`partitions.csv`中已定义为 `storage, data, spiffs, , 2M`。当前未挂载，SD卡使用FATFS独立挂载在 `/sdcard`。两者共存不冲突。

### 4. HTTP服务器端点

| 端点 | 返回 | 说明 |
|------|------|------|
| `GET /` | `text/html` | SPIFFS中的index.html |
| `GET /style.css` | `text/css` | 样式 |
| `GET /app.js` | `application/javascript` | 前端逻辑 |
| `GET /stream` | `multipart/x-mixed-replace` | MJPEG实时流 |
| `GET /api/status` | `application/json` | 系统状态 |
| `GET /api/snapshot` | `image/jpeg` | 单帧快照 |
| `GET /api/frames?tag_id=0x0001` | `application/json` | 列出指定资产的三视图 |
| `GET /api/image?tag_id=0x0001&view=front` | `image/jpeg` | 从SD卡提供指定JPEG |
| `GET /api/assets` | `application/json` | 列出所有已注册资产 |

### 5. MJPEG流设计（关键：摄像头互斥锁处理）

已有代码通过 `xCameraMutex`（`SemaphoreHandle_t`，通过 `xSemaphoreCreateMutex()` 创建）保护摄像头访问。AI推理管道在持有锁时耗时约1.2秒。

**策略：短超时 + 快捕获**

```c
// mjpeg_handler.c 核心循环
while (true) {
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    
    // 100ms超时 —— AI管道持锁时直接跳过本帧
    if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool ok = camera_module_capture_jpeg(&jpeg_buf, &jpeg_len);
        xSemaphoreGive(xCameraMutex);  // 立即释放
        
        if (ok) {
            // 发送MJPEG multipart数据
            // header: --frame\r\nContent-Type: image/jpeg\r\nContent-Length: X\r\n\r\n
            // body: JPEG data
            // tail: \r\n
            httpd_resp_send_chunk(req, part_buf, hdr_len);
            httpd_resp_send_chunk(req, (char*)jpeg_buf, jpeg_len);
            httpd_resp_send_chunk(req, "\r\n", 2);
            free(jpeg_buf);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(80));  // ~10fps上限
}
```

**互斥锁安全性保证**：
- AI管道优先级7 > HTTP服务器优先级5，AI总能抢占
- 流处理每次持锁仅30-80ms（一次JPEG抓取）
- 若AI正持锁，流处理100ms超时后跳过该帧，绝不阻塞
- 无死锁风险：持锁期间不获取其他锁，不调用延时

### 6. 系统状态API (`/api/status`)

返回JSON，数据来源：

```json
{
  "free_heap": 5234560,
  "min_free_heap": 5100000,
  "camera_ready": true,
  "storage_ready": true,
  "be_state": "idle",
  "current_tag_id": "0x0001",
  "wifi_clients": 1
}
```

- `free_heap` → `esp_get_free_heap_size()`
- `min_free_heap` → `esp_get_minimum_free_heap_size()`
- `camera_ready` → `g_camera_ready` (extern)
- `storage_ready` → `g_storage_ready` (extern)
- `be_state` → `be_get_state_string()` (business_executor.h)
- `current_tag_id` → `g_current_tag_id` (extern, main.h)
- `wifi_clients` → 通过WiFi事件统计连接/断开

### 7. FST帧图片API (`/api/frames`, `/api/image`)

帧图片保存在SD卡上，路径为：
- Tag ID格式: `/sdcard/assets/0x0001/0x0001_front.jpg`
- 旧MAC格式: `/sdcard/assets/AA_BB_CC_DD_EE_FF_front.jpg`

`/api/frames?tag_id=0x0001` 返回三视图是否存在及文件大小：
```json
{
  "tag_id": "0x0001",
  "frames": [
    {"view": "front", "url": "/api/image?tag_id=0x0001&view=front", "size": 12345},
    {"view": "side",  "url": "/api/image?tag_id=0x0001&view=side",  "size": 0},
    {"view": "top",   "url": "/api/image?tag_id=0x0001&view=top",   "size": 8901}
  ]
}
```
(`size=0` 表示该视图不存在)

`/api/image` 直接从SD卡读取JPEG文件，设置 `Content-Type: image/jpeg` 返回。

### 8. 前端页面设计

```
+------------------------------------------------------------+
|  ESP32-S3 CAM AI 实时预览                    v3.4           |
+----------------------+-------------------------------------+
|                      |  ● 系统状态                          |
|   [MJPEG实时画面]    |  ─────────────────                   |
|   320×240 ~10fps     |  堆内存:     5,234 KB               |
|                      |  最小堆:     5,100 KB               |
|   [📸 拍摄快照]      |  摄像头:     ✅ 就绪                |
|                      |  存储:       ✅ 就绪 (SD卡)          |
|                      |  业务状态:   idle                    |
|                      |  当前Tag:    0x0001                  |
|                      |  WiFi客户端: 1                       |
+----------------------+-------------------------------------+
|  📂 FST三视图帧图片                                       |
|  Tag ID: [0x0001 ▾] [加载]  (下拉框列出所有资产)          |
|  ┌──────────┐  ┌──────────┐  ┌──────────┐                |
|  │ 正面     │  │ 侧面     │  │ 顶部     │                |
|  │ [图片]   │  │ 无图片   │  │ [图片]   │                |
|  │ 12.3 KB  │  │          │  │  8.9 KB  │                |
|  └──────────┘  └──────────┘  └──────────┘                |
+------------------------------------------------------------+
```

- 实时画面：`<img src="/stream">` 浏览器原生支持MJPEG
- 状态面板：每2秒轮询 `/api/status` 更新
- 快照按钮：`window.open('/api/snapshot')` 新标签打开
- 帧画廊：页面加载时请求 `/api/assets` 填充Tag下拉框，选择后请求 `/api/frames` 加载三视图

### 9. 任务优先级与资源

| 组件 | 优先级 | 栈 | 说明 |
|------|--------|-----|------|
| camera_ai_task (现有) | 7 | 16KB | 不动 |
| HTTP服务器 | 5 | 8KB | 低于摄像头、高于推理 |
| inference_task (现有) | 4 | 8KB | 不动 |
| storage_task (现有) | 4 | 8KB | 不动 |
| WiFi/LWIP (系统) | 18/18 | 系统 | 最高，不受影响 |

无新增FreeRTOS任务 —— HTTP服务器由esp_http_server内部线程池处理。

### 10. sdkconfig改动

| 配置项 | 目标值 | 原因 |
|--------|--------|------|
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | **y** | WiFi/LWIP 内存走 PSRAM，释放内部 SRAM |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 8192 | MJPEG 分块发送更流畅 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 8192 | 同上 |
| `CONFIG_LWIP_MAX_SOCKETS` | 16 | HTTP 服务器 + MJPEG 流需要 |
| `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` | 4096 | WiFi 事件回调栈安全余量 |

> WiFi 缓冲区（static_rx/dynamic_rx/dynamic_tx/cache_tx）**保持默认值**，不覆盖。

## 实现步骤

1. **创建 `main/modules/web/` 目录和4个源文件**（web_server.h/c, mjpeg_handler.c, api_handlers.c）
2. **创建 `spiffs_web/` 目录和3个前端文件**（index.html, style.css, app.js）
3. **修改 `main/CMakeLists.txt`**：注册新源文件、添加include路径、添加 `esp_wifi esp_http_server esp_netif lwip spiffs` 到REQUIRES
4. **修改项目根 `CMakeLists.txt`**：在 `project()` 前添加 `spiffs_create_partition_image`
5. **修改 `main/main.c`**：添加 `#include "modules/web/web_server.h"` 和 `web_server_init()` 调用
6. **调整 `sdkconfig`**：按第10节表格配置（由用户手动操作）
7. **编译烧录测试**：`idf.py build flash monitor`（由用户手动操作）

## 验证方法

1. 上电后串口日志应显示WiFi AP启动成功、SPIFFS挂载成功、HTTP服务器启动
2. 手机/电脑连接WiFi `ESP32-CAM-AI`，密码 `12345678`
3. 浏览器访问 `http://192.168.4.1`
4. 确认实时画面流畅（非AI推理期间约8-12fps）
5. 确认状态面板数据正确刷新
6. 选择一个已注册的Tag ID，确认三视图帧图片正确显示
7. 进行一次资产注册操作，确认Web预览和AI管道共存不冲突

---

# LCD 裸屏实时预览方案

## 背景

当前摄像头拍摄时没有实时画面反馈，导致：
- 拍照取景不规范（物体不在画面中心、角度偏斜）
- 模糊检测只能事后过滤，不能帮助用户主动调整
- AI 识别准确度受拍摄质量影响大

Web 实时流（WiFi）仅用于调试，**实际应用不能使用**（赛题要求 WS63 做 AP/Host）。ESP32 直驱 LCD 裸屏是独立于 WS63 的纯本地方案，不违反赛题约束。

## 目标

在采集阶段，ESP32-S3 驱动一块 SPI LCD 显示摄像头实时画面，帮助用户规范取景。

## 推荐屏幕

| 型号 | 分辨率 | 尺寸 | 接口 | 参考价 | 备注 |
|------|--------|------|------|--------|------|
| ST7789 | 240×240 | 1.3" | SPI | ¥3-5 | 最小最便宜，方形 |
| ST7789V | 240×320 | 2.0" | SPI | ¥8-12 | 竖屏，接近 QVGA |
| **ILI9341** | **320×240** | **2.4"-2.8"** | SPI | ¥8-15 | ⭐ 分辨率完全匹配 |
| ST7796 | 320×480 | 3.5" | SPI | ¥15-25 | 分辨率高但传得慢 |

> 推荐 ILI9341 320×240 2.4"，与 OV5640 QVGA 输出点对点，无需缩放。

## GPIO 资源

已用引脚：
```
Camera:  4,5,6,7,8,9,10,11,12,13,15,16,17,18
SD Card: 38,39,40
UART0:   43,44    UART1:   47,21    UART2:   19,20
LED:     48
```

**空闲可用**：GPIO 14, 33, 34, 35, 36, 37, 41, 42

LCD 只需 5 根线（SPI 4-wire + 可选背光），推荐接线：

```
LCD SCLK  → GPIO 14   (SPI Clock)
LCD MOSI  → GPIO 33   (SPI Data)
LCD DC    → GPIO 34   (Data/Command)
LCD CS    → GPIO 35   (Chip Select)
LCD RST   → GPIO 36   (Reset)
LCD BL    → GPIO 37   (背光 PWM, 可选)
```

## 性能估算

ESP32-S3 有硬件 JPEG 解码器 (`esp_new_jpeg`)，一条帧链路：

| 步骤 | 耗时 | 说明 |
|------|------|------|
| JPEG 抓取 | ~20ms | `esp_camera_fb_get()` |
| JPEG→RGB565 硬件解码 | ~8ms | S3 硬件加速 |
| SPI DMA 传输 (150KB) | ~16ms | 40MHz SPI, 320×240×2 |
| vTaskDelay | ~10ms | 帧间隔 |
| **每帧合计** | **~54ms** | **≈ 18fps 理论峰值** |

实测约 12-15fps，流畅可用。低于 320×240 分辨率可更快。

## 软件架构

### 文件结构

```
main/modules/display/
├── display_module.h       # 公开 API
├── display_module.c       # 初始化 + FreeRTOS 预览任务
└── lcd_driver.c           # ILI9341/ST7789 SPI 驱动

components/esp_new_jpeg/   # 已有（ESP-IDF 硬件 JPEG 解码器）
```

### 预览任务

```c
#define DISPLAY_TASK_STACK  4096
#define DISPLAY_TASK_PRIO   3      // 低于所有业务任务

void display_preview_task(void *arg) {
    uint16_t *fb_rgb565 = heap_caps_malloc(320 * 240 * 2,
                                            MALLOC_CAP_SPIRAM);

    while (1) {
        if (!g_preview_enabled) {
            lcd_backlight(0);       // 关背光
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        lcd_backlight(1);

        // 拿摄像头锁（短超时，不阻塞 AI）
        if (xSemaphoreTake(xCameraMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
            camera_fb_t *fb = esp_camera_fb_get();
            xSemaphoreGive(xCameraMutex);

            if (fb && fb->format == PIXFORMAT_JPEG) {
                // 硬件 JPEG → RGB565
                jpeg_decode_to_rgb565(fb->buf, fb->len,
                                      fb_rgb565, 320, 240);
                // SPI DMA 发送到 LCD
                lcd_write_frame(fb_rgb565, 320, 240);
            }
            if (fb) esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

### 预览开关策略

`g_preview_enabled` 在以下条件为 true 时自动开启：
- `g_camera_ready == true`
- 业务执行器状态为 `WAITING_CAPTURE` 或 `CAPTURING`

不需要手动控制，也不占用 UART 通信。

## 集成点

改动极小，仅涉及：

| 文件 | 改动 |
|------|------|
| `main/modules/display/` (新) | 3 个源文件 |
| `main/CMakeLists.txt` | +1 源文件、+1 include 目录 |
| `main/main.c` | +1 行 `display_module_init()` |

**不修改 camera_module、AI pipeline、UART 协议、WS63、业务逻辑。**

## 实施步骤

| 步骤 | 内容 | 预估 |
|------|------|------|
| 1 | 购买 ILI9341 2.4" SPI LCD 模块 | 淘宝 2-3 天 |
| 2 | 焊接/杜邦线连接 5 根 GPIO | 10 分钟 |
| 3 | 编写 LCD 驱动（SPI 初始化 + 写帧函数） | 30 分钟 |
| 4 | 编写预览任务 + JPEG 解码 | 30 分钟 |
| 5 | 集成到 main.c + CMakeLists | 5 分钟 |
| 6 | 编译烧录测试 | 10 分钟 |

> 注：ESP-IDF 内置 `esp_lcd` 驱动框架支持 ILI9341/ST7789，可直接用 `components/espressif__esp_lcd_ili9341` 通过 IDF 组件管理器安装，无需自己写驱动。如果自己写精简版驱动也更简单（核心只需 `spi_device_transmit` + 几个寄存器初始化命令）。

## 验证方法

1. 上电后 LCD 背光亮起，显示摄像头实时画面
2. 进行资产注册操作，采集阶段预览流畅（≥10fps）
3. 采集完成后背光自动关闭
4. 确认 AI 推理管道不受影响（互斥锁超时保证）
5. 对比有无预览时的拍摄质量（取景规范性、识别准确度）
