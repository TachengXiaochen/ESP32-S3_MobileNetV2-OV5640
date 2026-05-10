# WS63 ↔ ESP32-S3 ↔ L610 三方通信协议规范

> **文档版本**: v1.0  
> **创建日期**: 2026-05-10  
> **适用项目**: CAM_AI (ESP32-S3 + L610 4G模块集成)  
> **关联文档**: [WS63_ESP32_PROTOCOL.md](WS63_ESP32_PROTOCOL.md) v3.1  

---

## 目录

1. [系统架构](#1-系统架构)
2. [硬件连接](#2-硬件连接)
3. [L610下行命令 (WS63 → ESP32)](#3-l610下行命令-ws63--esp32)
4. [L610上行消息 (ESP32 → WS63)](#4-l610上行消息-esp32--ws63)
5. [MQTT业务流程](#5-mqtt业务流程)
6. [主动上报机制](#6-主动上报机制)
7. [AT指令透传](#7-at指令透传调试)
8. [错误处理与重试](#8-错误处理与重试)
9. [配置参数](#9-配置参数)
10. [测试验证](#10-测试验证)

---

## 1. 系统架构

```
┌──────────────┐         UART1          ┌──────────────┐         UART2          ┌──────────────┐
│     WS63      │ ◄────────────────────► │   ESP32-S3   │ ◄────────────────────► │    L610      │
│  (主控/Host)  │   JSON Commands        │  (网关/GW)   │   AT Commands          │  (4G Module) │
│              │   cJSON Protocol       │              │   URC Events           │              │
└──────────────┘                        └──────────────┘                        └──────────────┘
     │                                        │                                        │
     │                                        │                                        │
     ▼                                        ▼                                        ▼
  业务逻辑                              协议转换                               MQTT云端通信
  UI交互                                状态管理                               4G网络连接
  云平台对接                            资源调度                               AT指令执行
```

**核心职责分工**：

| 组件 | 职责 | 通信接口 |
|------|------|----------|
| **WS63** | 业务编排、用户交互、结果展示 | UART1 (JSON) |
| **ESP32-S3** | 协议转换、状态管理、资源调度 | UART1 (JSON) + UART2 (AT) |
| **L610** | 4G网络连接、MQTT通信、AT指令执行 | UART2 (AT) |

---

## 2. 硬件连接

### 2.1 ESP32-S3 ↔ L610 连接

| 信号 | ESP32-S3 引脚 | L610 引脚 | 方向 | 说明 |
|------|-------------|----------|------|------|
| UART TX | **GPIO4** | RX | ESP32 → L610 | AT指令发送 |
| UART RX | **GPIO5** | TX | L610 → ESP32 | URC事件接收 |
| GND | GND | GND | — | 共地（必须） |
| VCC | 3.3V/5V | VCC | — | 供电（根据模块规格） |

> ⚠️ **重要提示**：
> - L610模块峰值电流可达2A，建议使用独立电源或确保供电稳定
> - 必须连接4G天线，否则信号极弱
> - SIM卡必须已激活且有流量套餐

### 2.2 UART配置参数

| 参数 | 值 |
|------|-----|
| 接口 | UART_NUM_2 |
| 波特率 | **115200** bps |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | 无 |
| 流控 | 无 |
| 缓冲区大小 | 2048 字节 |

---

## 3. L610下行命令 (WS63 → ESP32)

### 3.1 mqtt_connect - 连接MQTT服务器

**命令格式**：
```json
{
  "cmd": "mqtt_connect",
  "host": "mqtt.thingskit.com",
  "port": 1883,
  "clean_session": 1,
  "keepalive": 60
}
```

**字段说明**：
| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| cmd | string | ✅ | - | 固定值"mqtt_connect" |
| host | string | ✅ | - | MQTT服务器地址 |
| port | number | ❌ | 1883 | 端口号 |
| clean_session | number | ❌ | 1 | 清除会话标志（0/1） |
| keepalive | number | ❌ | 60 | 心跳间隔（秒），范围1-300 |

**前置条件**：
- ✅ L610模块已初始化（状态为READY）
- ✅ 网络附着成功（AT+CGATT?返回1）
- ✅ 信号质量良好（AT+CSQ > 10）

**执行流程**：
1. ESP32检查L610模块状态
2. 动态生成ClientID：`WS63-{MAC}`（在register时已生成）
3. 调用`l610_mqtt_set_user()`设置MQTT凭据
4. 调用`l610_mqtt_connect()`发起连接
5. 等待L610返回+MQTTOPEN URC事件（超时15秒）
6. 向WS63上报连接结果

**响应示例**：
```json
// 成功
{
  "type": "mqtt_connected",
  "state": "connected",
  "host": "mqtt.thingskit.com",
  "port": 1883
}

// 失败
{
  "type": "mqtt_error",
  "code": "MQTT_CONNECT_FAILED",
  "msg": "MQTT connection failed"
}
```

---

### 3.2 mqtt_publish - 发布MQTT消息

**命令格式**：
```json
{
  "cmd": "mqtt_publish",
  "topic": "device/status",
  "payload": "{\"mac\":\"AA:BB:CC:DD:EE:FF\",\"status\":\"online\"}",
  "qos": 1,
  "retain": 0
}
```

**字段说明**：
| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| cmd | string | ✅ | - | 固定值"mqtt_publish" |
| topic | string | ✅ | - | MQTT主题（建议≤128字节） |
| payload | string | ✅ | - | 消息内容 |
| qos | number | ❌ | 1 | QoS等级（0/1/2） |
| retain | number | ❌ | 0 | 保留标志（0/1） |

**⚠️ 限制条件**：
- **Payload最大长度：1024字节**（超过返回ERR_INVALID_SIZE）
- 必须在MQTT连接状态下执行
- AT指令总长度不能超过UART缓冲区（2048字节）

**执行流程**：
1. 检查payload长度（≤1024字节）
2. 估算AT指令总长度：`AT+MQTTPUB=1,"{topic}",{qos},{retain},"{payload}"`
3. 构造AT+MQTTPUB指令并发送至L610
4. 等待+MQTTPUB URC事件（超时10秒）
5. 向WS63上报发布结果

**响应示例**：
```json
// 成功
{
  "type": "mqtt_publish_result",
  "result": "ok",
  "topic": "device/status"
}

// Payload超长
{
  "type": "error",
  "code": "ERR_INVALID_SIZE",
  "msg": "Payload too long: 1025 bytes (max 1024)"
}
```

---

### 3.3 mqtt_disconnect - 断开MQTT连接

**命令格式**：
```json
{"cmd": "mqtt_disconnect"}
```

**执行流程**：
1. 调用`l610_mqtt_disconnect()`
2. 发送AT+MQTTCLOSE指令
3. 等待L610返回OK
4. 内部状态变为DISCONNECTED
5. 向WS63上报断开结果

**响应示例**：
```json
{
  "type": "mqtt_connected",
  "state": "disconnected",
  "host": "mqtt.thingskit.com",
  "port": 1883
}
```

---

### 3.4 l610_at - AT指令透传（调试用）

**命令格式**：
```json
{"cmd": "l610_at", "at": "AT+CSQ"}
```

**字段说明**：
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| cmd | string | ✅ | 固定值"l610_at" |
| at | string | ✅ | AT指令（不含\r\n） |

**用途**：
- 🔧 调试L610模块状态
- 📊 查询信号质量、网络附着等
- ✅ 验证模块通信正常

**常用AT指令**：
```bash
AT              # 测试通信
AT+CSQ          # 信号质量（返回：+CSQ: 25,99）
AT+CGATT?       # 网络附着状态（返回：+CGATT: 1）
AT+CREG?        # 注册状态（返回：+CREG: 0,1）
AT+MQTTLOG=1    # 开启MQTT详细日志
```

**响应示例**：
```json
// 成功
{
  "type": "l610_at_result",
  "cmd": "AT+CSQ",
  "result": "ok",
  "response": "+CSQ: 25,99\r\nOK"
}

// 超时
{
  "type": "l610_error",
  "code": "L610_AT_TIMEOUT",
  "msg": "L610 not responding"
}
```

---

### 3.5 l610_status - 查询L610状态

**命令格式**：
```json
{"cmd": "l610_status"}
```

**返回信息**：
```json
{
  "type": "l610_status",
  "l610_state": "READY",
  "mqtt_state": "CONNECTED",
  "signal_quality": 25,
  "network_attached": true,
  "current_host": "mqtt.thingskit.com",
  "current_port": 1883
}
```

**字段说明**：
| 字段 | 类型 | 说明 |
|------|------|------|
| l610_state | string | OFF/READY/ERROR |
| mqtt_state | string | DISCONNECTED/CONNECTED |
| signal_quality | number | CSQ值（0-31，99表示未知） |
| network_attached | boolean | 是否附着GPRS网络 |
| current_host | string | 当前连接的MQTT服务器 |
| current_port | number | 当前端口号 |

---

## 4. L610上行消息 (ESP32 → WS63)

### 4.1 mqtt_connected - MQTT连接成功

**消息格式**：
```json
{
  "type": "mqtt_connected",
  "state": "connected",
  "host": "mqtt.thingskit.com",
  "port": 1883
}
```

**触发时机**：
- `mqtt_connect`命令执行成功
- 收到L610的+MQTTOPEN: 1,0 URC事件

---

### 4.2 mqtt_error - MQTT错误

**消息格式**：
```json
{
  "type": "mqtt_error",
  "code": "MQTT_CONNECT_FAILED",
  "msg": "MQTT connection failed"
}
```

**常见错误码**：
| code | 说明 | 可能原因 |
|------|------|----------|
| MQTT_CONNECT_FAILED | 连接失败 | 网络异常/服务器不可达/认证失败 |
| MQTT_PUBLISH_FAILED | 发布失败 | QoS超时/主题无效/Payload过长 |
| MQTT_DISCONNECTED | 意外断开 | 网络中断/服务器关闭连接/心跳超时 |

---

### 4.3 l610_error - L610模块错误（⭐主动上报）

**消息格式**：
```json
{
  "type": "l610_error",
  "code": "L610_NOT_RESPONDING",
  "msg": "L610 module not responding"
}
```

**触发场景**：

#### 场景1：模块失联
- **检测方式**：心跳任务连续3次AT指令超时
- **错误码**：`L610_NOT_RESPONDING`
- **日志**：`W (xxx) l610_driver: AT timeout after 5000 ms (consecutive=3/3, LOST)`

#### 场景2：MQTT意外断开
- **检测方式**：收到L610的+MQTTBROKEN URC事件
- **错误码**：`MQTT_DISCONNECTED`
- **日志**：`W (xxx) l610_manager: MQTT disconnected (URC received)`

#### 场景3：网络异常
- **检测方式**：AT+CGATT?返回0
- **错误码**：`NETWORK_DETACHED`
- **日志**：`W (xxx) l610_manager: Network detached`

**重要性**：
- ✅ 这是v3.1新增的**主动上报机制**
- ✅ L610管理器通过回调函数主动向WS63发送
- ✅ 无需WS63轮询，实时性更高

**实现原理**：
```c
// protocol_handler.c中注册回调
void protocol_handler_init(void) {
    extern void l610_manager_register_send_func(void (*)(const char *));
    l610_manager_register_send_func(ws63_send_json_raw);
    ESP_LOGI(TAG, "L610 manager send callback registered");
}

// l610_manager.c中检测到错误时调用
static void report_l610_error(const char *code, const char *msg) {
    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf),
             "{\"type\":\"l610_error\",\"code\":\"%s\",\"msg\":\"%s\"}",
             code, msg);
    
    if (g_ws63_send_func) {
        g_ws63_send_func(json_buf);  // 主动上报
    }
}
```

---

### 4.4 l610_at_result - AT指令结果

**消息格式**：
```json
{
  "type": "l610_at_result",
  "cmd": "AT+CSQ",
  "result": "ok",
  "response": "+CSQ: 25,99\r\nOK"
}
```

**字段说明**：
| 字段 | 类型 | 说明 |
|------|------|------|
| type | string | 固定值"l610_at_result" |
| cmd | string | 原始AT指令 |
| result | string | "ok"或"error" |
| response | string | L610返回的完整响应（包含\r\n） |

---

## 5. MQTT业务流程

### 5.1 完整连接流程

```
WS63                          ESP32                         L610
 │                              │                              │
 │── mqtt_connect ──────────►│                              │
 │                              │── AT+MQTTUSER ──────────►│
 │                              │◄── OK ───────────────────│
 │                              │── AT+MQTTOPEN ──────────►│
 │                              │◄── +MQTTOPEN: 1,0 ──────│
 │◄── mqtt_connected ───────│                              │
 │                              │                              │
 │── mqtt_publish ──────────►│                              │
 │                              │── AT+MQTTPUB ───────────►│
 │                              │◄── +MQTTPUB: 1,0 ───────│
 │◄── mqtt_publish_result ──│                              │
 │                              │                              │
 │── mqtt_disconnect ───────►│                              │
 │                              │── AT+MQTTCLOSE ─────────►│
 │                              │◄── OK ───────────────────│
 │◄── mqtt_disconnected ────│                              │
```

### 5.2 ClientID生成规则

**格式**：`WS63-{MAC}`

**示例**：
- MAC地址：`AA:BB:CC:DD:EE:FF`
- ClientID：`WS63-AA:BB:CC:DD:EE:FF`

**生成时机**：
- 在`register`命令执行时生成
- 存储在`g_l610_client_id`全局变量
- 每次`mqtt_connect`时自动使用

**优势**：
- ✅ 云端可通过ClientID识别设备来源
- ✅ 每个设备有唯一标识
- ✅ 符合物联网设备命名规范

**代码实现**：
```c
// protocol_handler.c:L836-838
snprintf(g_l610_client_id, sizeof(g_l610_client_id), "WS63-%s", g_ws63_mac);
ESP_LOGI(TAG, "L610 ClientID generated: %s", g_l610_client_id);

// protocol_handler.c:L1887-1890
extern esp_err_t l610_mqtt_set_user(const char *, const char *, const char *);
if (strlen(g_l610_client_id) > 0) {
    l610_mqtt_set_user(g_l610_client_id, NULL, NULL);
    ESP_LOGI(TAG, "L610 MQTT user set with ClientID: %s", g_l610_client_id);
}
```

---

## 6. 主动上报机制 ⭐NEW v3.1

### 6.1 机制说明

v3.1版本新增了L610模块**主动向WS63上报**的能力，无需WS63轮询查询。

**实现方式**：
1. WS63在`protocol_handler_init()`中注册回调函数
2. L610管理器保存该回调函数指针
3. 检测到URC事件或错误时，直接调用回调发送JSON

**代码实现**：
```c
// protocol_handler.c:L180
void protocol_handler_init(void) {
    // ... 其他初始化 ...
    
    // 注册L610主动上报回调
    extern void l610_manager_register_send_func(void (*)(const char *));
    l610_manager_register_send_func(ws63_send_json_raw);
    ESP_LOGI(TAG, "L610 manager send callback registered");
}

// l610_manager.c中检测到错误时
static void report_l610_error(const char *code, const char *msg) {
    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf),
             "{\"type\":\"l610_error\",\"code\":\"%s\",\"msg\":\"%s\"}",
             code, msg);
    
    if (g_ws63_send_func) {
        g_ws63_send_func(json_buf);  // 主动上报
    }
}
```

### 6.2 上报场景

| 场景 | 触发条件 | 错误码 | 上报延迟 |
|------|---------|--------|----------|
| 模块失联 | 连续3次AT超时 | L610_NOT_RESPONDING | ≤90秒（心跳间隔30秒×3） |
| MQTT断开 | 收到+MQTTBROKEN URC | MQTT_DISCONNECTED | <1秒（URC即时处理） |
| 网络异常 | CGATT状态变为0 | NETWORK_DETACHED | ≤30秒（下次心跳检测） |

### 6.3 与传统轮询对比

| 维度 | 传统轮询 | 主动上报（v3.1） |
|------|---------|-----------------|
| 实时性 | 低（依赖轮询间隔） | 高（事件驱动） |
| 网络负载 | 高（频繁查询） | 低（仅事件时上报） |
| 功耗 | 高（持续查询） | 低（按需上报） |
| 复杂度 | 简单 | 中等（需回调机制） |
| 可靠性 | 中（可能漏检） | 高（即时通知） |

---

## 7. AT指令透传调试

### 7.1 常用AT指令速查

| 指令 | 说明 | 预期响应 | 用途 |
|------|------|----------|------|
| `AT` | 测试通信 | `OK` | 验证模块在线 |
| `AT+CSQ` | 信号质量 | `+CSQ: 25,99` | 检查信号强度 |
| `AT+CGATT?` | 网络附着 | `+CGATT: 1` | 确认GPRS附着 |
| `AT+CREG?` | 注册状态 | `+CREG: 0,1` | 检查网络注册 |
| `AT+CGDCONT?` | PDP上下文 | `+CGDCONT: 1,"IP","cmnet"` | 查看APN配置 |
| `AT+MQTTLOG=1` | MQTT日志 | `OK` | 开启详细日志 |
| `AT+MQTTUSER` | 设置MQTT用户 | `OK` | 配置认证信息 |
| `AT+MQTTOPEN` | 打开MQTT连接 | `+MQTTOPEN: 1,0` | 手动连接测试 |
| `AT+MQTTPUB` | 发布消息 | `+MQTTPUB: 1,0` | 手动发布测试 |
| `AT+MQTTCLOSE` | 关闭MQTT连接 | `OK` | 手动断开测试 |

### 7.2 调试流程

#### 步骤1：验证模块通信
```json
{"cmd":"l610_at","at":"AT"}
```
**预期**：返回`OK`

#### 步骤2：检查信号质量
```json
{"cmd":"l610_at","at":"AT+CSQ"}
```
**预期**：CSQ > 10（信号良好）

#### 步骤3：确认网络附着
```json
{"cmd":"l610_at","at":"AT+CGATT?"}
```
**预期**：`+CGATT: 1`

#### 步骤4：开启MQTT日志
```json
{"cmd":"l610_at","at":"AT+MQTTLOG=1"}
```
**用途**：后续连接失败时查看详细错误

---

## 8. 错误处理与重试

### 8.1 AT指令重试机制 ⭐NEW v3.1

**问题**：单次AT超时直接返回错误，网络波动时容易误判模块失联

**解决方案**：
- 最多重试3次（`L610_AT_MAX_RETRY = 3`）
- 重试间隔200ms
- 根据连续超时次数动态调整日志级别

**实现代码**：
```c
// l610_driver.c:L158-230
for (int retry = 0; retry < L610_AT_MAX_RETRY; retry++) {
    // 发送AT指令并等待应答
    // ... 
    
    if (strstr(response_buf, "OK\r\n") != NULL || ...) {
        g_consecutive_timeouts = 0;
        if (retry > 0) {
            ESP_LOGI(TAG, "AT command succeeded after %d retries: %s", 
                     retry, cmd);
        }
        return ESP_OK;
    }
    
    if (retry < L610_AT_MAX_RETRY - 1) {
        ESP_LOGD(TAG, "AT timeout (retry %d/%d): %s", 
                 retry + 1, L610_AT_MAX_RETRY - 1, cmd);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// 所有重试失败后的智能日志
if (g_consecutive_timeouts >= L610_LOST_THRESHOLD) {
    ESP_LOGW(TAG, "AT timeout after %lu ms (consecutive=%d/%d, LOST): %s", ...);
} else if (g_consecutive_timeouts >= 2) {
    ESP_LOGW(TAG, "AT timeout after %lu ms (consecutive=%d): %s", ...);
} else {
    ESP_LOGD(TAG, "AT timeout (1st): %s", cmd);
}
```

**日志策略**：
| 连续超时次数 | 日志级别 | 说明 |
|------------|---------|------|
| 1次 | DEBUG | 首次超时，可能是偶发 |
| 2次 | WARNING | 连续超时，需要关注 |
| ≥3次 | ERROR | 达到阈值，标记LOST |

### 8.2 Payload长度保护 ⭐NEW v3.1

**问题**：未检查payload长度，可能导致AT指令超长被拒绝或缓冲区溢出

**解决方案**：
- Payload限制：最大1024字节
- AT指令总长度估算：防止超过UART缓冲区（2048字节）

**实现代码**：
```c
// l610_mqtt.c:L180-193
size_t payload_len = strlen(payload);
if (payload_len > 1024) {
    ESP_LOGE(TAG, "Payload too long: %zu bytes (max 1024)", payload_len);
    return ESP_ERR_INVALID_SIZE;
}

size_t estimated_cmd_len = strlen("AT+MQTTPUB=1,\"") + strlen(topic) + 
                           strlen("\",1,0,\"") + payload_len + strlen("\"");
if (estimated_cmd_len >= L610_UART_BUF_SIZE - 1) {
    ESP_LOGE(TAG, "AT command too long: %zu bytes (max %d)", 
             estimated_cmd_len, L610_UART_BUF_SIZE - 1);
    return ESP_ERR_INVALID_SIZE;
}
```

### 8.3 资源清理机制 ⭐NEW v3.1

**问题**：L610 MQTT模块创建的信号量在模块停止时未销毁，长期运行导致资源泄漏

**解决方案**：
- 新增`l610_mqtt_cleanup()`函数
- 在`l610_manager_stop()`中调用清理函数
- 销毁所有信号量并重置状态

**实现代码**：
```c
// l610_mqtt.c:L276-293
void l610_mqtt_cleanup(void)
{
    if (g_mqtt_open_sem) {
        vSemaphoreDelete(g_mqtt_open_sem);
        g_mqtt_open_sem = NULL;
    }
    if (g_mqtt_pub_sem) {
        vSemaphoreDelete(g_mqtt_pub_sem);
        g_mqtt_pub_sem = NULL;
    }
    if (g_mqtt_close_sem) {
        vSemaphoreDelete(g_mqtt_close_sem);
        g_mqtt_close_sem = NULL;
    }
    
    g_mqtt_state = MQTT_STATE_DISCONNECTED;
    g_user_set = false;
    memset(g_current_host, 0, sizeof(g_current_host));
    g_current_port = 0;
    
    ESP_LOGI(TAG, "L610 MQTT resources cleaned up");
}

// l610_manager.c:L265-268
esp_err_t l610_manager_stop(void)
{
    g_manager_running = false;
    if (g_heartbeat_task) {
        vTaskDelete(g_heartbeat_task);
        g_heartbeat_task = NULL;
    }
    
    // ✅ 清理MQTT资源（信号量等）
    extern void l610_mqtt_cleanup(void);
    l610_mqtt_cleanup();
    
    return ESP_OK;
}
```

---

## 9. 配置参数

### 9.1 L610模块配置（l610_config.h）

```c
// UART配置
#define L610_UART_NUM        UART_NUM_2
#define L610_UART_TX_PIN     GPIO_NUM_4
#define L610_UART_RX_PIN     GPIO_NUM_5
#define L610_UART_BAUD       115200
#define L610_UART_BUF_SIZE   2048

// MQTT配置
#define L610_MQTT_USERNAME   "IekgXZSavYJ6KEJFyvb4"
#define L610_MQTT_PASSWORD   ""
#define L610_MQTT_KEEPALIVE  60
#define L610_MQTT_QOS        1
#define L610_MQTT_DEFAULT_PORT 1883

// 重试配置
#define L610_AT_MAX_RETRY    3           // AT指令最大重试次数
#define L610_AT_DEFAULT_TIMEOUT 5000     // AT指令默认超时（毫秒）
#define L610_LOST_THRESHOLD  3           // 连续超时阈值（标记LOST）

// 心跳配置
#define L610_HEARTBEAT_INTERVAL_SEC 30   // 心跳检测间隔（秒）
```

### 9.2 性能指标参考

| 指标 | 正常范围 | 说明 |
|------|---------|------|
| CSQ信号质量 | > 15 | 越大越好（0-31） |
| AT指令响应时间 | < 1秒 | 网络良好时 |
| MQTT连接时间 | < 15秒 | 包含网络注册 |
| MQTT发布延迟 | < 2秒 | QoS=1时 |
| Heap空闲内存 | > 50KB | 避免碎片化 |
| 心跳间隔 | 30秒 | 可配置 |

---

## 10. 测试验证

### 10.1 快速测试清单

在部署前，请完成以下测试：

- [ ] L610模块通信正常（AT指令响应）
- [ ] 信号质量良好（CSQ > 15）
- [ ] 网络附着成功（CGATT=1）
- [ ] MQTT连接成功
- [ ] 消息发布成功（至少10条）
- [ ] Payload长度保护生效（1025字节拒绝）
- [ ] 断开连接正常
- [ ] 重连功能正常（至少3次）
- [ ] 异常上报机制工作（断开网线测试）
- [ ] 内存无泄漏（运行1小时heap稳定）

### 10.2 详细测试指南

请参考完整的调试指南：**[L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md)**

包含：
- 环境准备（硬件连接、软件配置）
- 基础功能验证（AT指令、信号质量、网络附着）
- MQTT业务测试（连接、发布、断开）
- 异常场景测试（意外断开、模块失联、重连恢复）
- 性能压力测试（连续发布、重连稳定性）
- 故障排查（6种常见问题及解决方案）

---

## 附录

### A. 错误码对照表

| 错误码 | 说明 | 解决方案 |
|--------|------|----------|
| `ERR_INVALID_ARG` | 参数错误 | 检查JSON字段是否完整 |
| `ERR_INVALID_SIZE` | 长度超限 | Payload不超过1024字节 |
| `ERR_TIMEOUT` | 超时 | 检查网络和模块状态 |
| `L610_NOT_RESPONDING` | 模块失联 | 检查UART接线和供电 |
| `MQTT_DISCONNECTED` | MQTT断开 | 检查网络和服务器状态 |
| `MQTT_CONNECT_FAILED` | 连接失败 | 验证MQTT参数和认证信息 |
| `NETWORK_DETACHED` | 网络脱离 | 检查SIM卡和网络覆盖 |

### B. 相关文档

- **主协议规范**：[WS63_ESP32_PROTOCOL.md](WS63_ESP32_PROTOCOL.md) v3.1
- **调试指南**：[L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md)
- **版本更新日志**：[CHANGELOG_v3.1.md](CHANGELOG_v3.1.md)
- **项目README**：[README.md](../README.md)

---

**文档版本**: v1.0  
**创建日期**: 2026-05-10  
**维护者**: CAM_AI开发团队  
**反馈邮箱**: support@cam-ai.com
