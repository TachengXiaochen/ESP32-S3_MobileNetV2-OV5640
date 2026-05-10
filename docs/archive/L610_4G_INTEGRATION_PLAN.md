# L610 4G模块集成方案

> **文档版本**: v2.0  
> **最后更新**: 2026-05-09  
> **适用项目**: CAM_AI (物资管理子节点)  
> **涉及硬件**: WS63 (主控) + ESP32-S3 (视觉感知) + **ADP-L610-Arduino开发板** (4G代理)

---

## 目录

1. [系统架构](#1-系统架构)
2. [硬件连接](#2-硬件连接)
3. [L610 开机时序](#3-l610-开机时序)
4. [L610 MQTT AT指令集(已确认)](#4-l610-mqtt-at指令集已确认)
5. [ESP32 新增软件模块](#5-esp32-新增软件模块)
6. [WS63 协议扩展](#6-ws63-协议扩展)
7. [数据流设计](#7-数据流设计)
8. [ThingsKit 物模型建议](#8-thingskit-物模型建议)
9. [开发实施路线](#9-开发实施路线)
10. [附录：Phase 0 MQTT测试脚本](#10-附录phase-0-mqtt测试脚本)

---

## 1. 系统架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        云端 (ThingsKit MQTT Broker)              │
│                    broker: demo.thingskit.com:1883               │
│                    username: IekgXZSavYJ6KEJFyvb4               │
│                    password: (空)                                │
└─────────────────────────────────┬────────────────────────────────┘
                                  │ MQTT (统一JSON格式)
                    ┌─────────────┴─────────────┐
                    │     WiFi通路              │     4G通路 (AT+MQTTPUB)
                    │  WS63原生MQTT库           │     L610模块代理
                    ▼                           ▼
┌──────────────────────────────────────────────────────────────────┐
│                  WS63 (主控, LiteOS)                             │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  [数据聚合层] 收到ESP32的task_done → 组装统一云端JSON     │   │
│  │  [路径决策层] WiFi可用? → MQTT over WiFi / 否则走4G      │   │
│  │  [WiFi MQTT]  直连ThingsKit                              │   │
│  │  [命令转发层]  将4G上传需求 → 通过UART1下发到ESP32        │   │
│  └──────────────────────────────────────────────────────────┘   │
│  UART0(调试)    UART1(→ ESP32)    UART2(→ 串口屏)               │
└──────────────────────────┬───────────────────────────────────────┘
                           │ UART1 (JSON Lines, 115200)
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│              ESP32-S3 (视觉感知 + L610代理)                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  [已有]  camera / AI / storage / 协议处理                │   │
│  │  [新增]  L610驱动模块 (l610_driver.c)                   │   │
│  │  [新增]  L610 MQTT通道 (l610_mqtt.c)                   │   │
│  │  [新增]  4G上传代理 (将WS63的AT指令转发到L610)           │   │
│  └──────────────────────────────────────────────────────────┘   │
│  UART0(调试PC)    UART1(→ WS63)    UART2(→ ADP-L610开发板)     │
│                                         │ AT指令                │
│                                         │ J3 (TXD/RXD/GND)     │
│                                         │    3.3V电平 ✅        │
│                                         ▼                       │
│               ┌──────────────────────────────────┐              │
│               │   ADP-L610-Arduino 开发板         │              │
│               │   ┌────────────────────────┐     │              │
│               │   │   L610 MiniPCIe 模块    │     │              │
│               │   │   · 板载FPC天线         │     │              │
│               │   │   · 贴片IoT SIM卡       │     │              │
│               │   │   · 板载电平转换        │     │              │
│               │   │   · USB TYPE-C (PC调试) │     │              │
│               │   └────────────────────────┘     │              │
│               │   供电: USB 5V / DC 5V           │              │
│               └──────────────────────────────────┘              │
└──────────────────────────────────────────────────────────────────┘
```

### 核心原则

| 原则 | 说明 |
|------|------|
| **统一JSON** | 无论WiFi还是4G，发给云端的MQTT消息Payload完全一致 |
| **WS63是唯一决策者** | 路径选择（WiFi / 4G）由WS63决定，ESP32不感知 |
| **L610是透明4G管道** | L610只负责TCP/IP + MQTT AT指令透传，不做业务逻辑 |
| **分层解耦** | WS63↔ESP32 内部UART协议 与 WS63↔云端 MQTT协议 是独立的两层 |

---

## 2. 硬件连接

### 2.1 ADP-L610-Arduino 开发板资源确认

| 项目 | 状态 | 说明 |
|------|------|------|
| L610模组 | ✅ 已有 | 板载MiniPCIe L610模块 |
| SIM卡 | ✅ 已有 | 板载贴片IoT SIM卡（3年流量，每月100M）+ 额外SIM卡座 |
| 天线 | ✅ 已有 | 板载FPC天线（已贴在泡棉上） |
| 电源 | ✅ 已有 | USB 5V供电（板载DC/DC→3.8V给L610） |
| 电平转换 | ✅ 已确认3.3V | J3串口电平实测为3.3V，可直接连ESP32 |
| USB调试 | ✅ 可用 | TYPE-C直连PC，USB虚拟串口发AT指令 |

### 2.2 ESP32-S3 ↔ ADP-L610 连接

**ADP板 J3 排针定义：**

| J3引脚 | 信号方向 | 电平 | 说明 |
|--------|----------|------|------|
| J3-1 (TXD) | L610发送 → 外部接收 | **3.3V** ✅ | L610的UART发送 |
| J3-2 (RXD) | L610接收 ← 外部发送 | **3.3V** ✅ | L610的UART接收 |
| J3-3 (GND) | — | — | 共地 |

**连接图：**

```
ESP32-S3                            ADP-L610-Arduino
┌────────────┐                     ┌──────────────────┐
│ UART2 TX   │ GPIO19 ────────────►│ J3-2 (RXD)       │
│            │          (3.3V→3.3V)│                  │
│ UART2 RX   │ GPIO20 ◄────────────│ J3-1 (TXD)       │
│            │          (3.3V→3.3V)│                  │
│ GND        │ ────────────────────│ J3-3 (GND)       │
└────────────┘                     │                  │
                                   │ USB TYPE-C ← PC  │
                                   │  (独立供电+调试)   │
                                   └──────────────────┘
```

> ⚠️ **重要注意事项：**
> - **两个板各自USB供电**（共地即可），不必从一方取电
> - **Phase 1前先做Phase 0** — 用PC通过USB验证AT指令，确认L610正常工作后再连J3
> - 如果连上后通信不稳定，考虑在TX/RX上各串一个100Ω电阻
> - 避免在ESP32和ADP板同时上电时插拔J3

### 2.3 ADP板独立调试口（Phase 0用）

```
PC ←──USB──→ ADP-L610-Arduino
              (USB虚拟串口，直接发AT指令)
```

USB驱动安装后，设备管理器中会出现多个COM口：
| 端口 | 功能 |
|------|------|
| COMx (Port 0) | **AT命令口**（最常用，发AT指令，115200 8N1） |
| COMx (Port 1) | Diag口（调试，FIBOCOM内部使用） |
| COMx (Port 5) | 另一个AT命令口 |

> 测试时建议用 **Port 0**，波特率 **115200**，AT指令以 `\r` (0x0D) 结尾。

### 2.4 ADP板开关设置

| 开关 | 丝印 | 默认 | 说明 |
|------|------|------|------|
| SW3 | 供电选择 | B5V (Arduino口) | 改为 **USB**（用USB供电时） |
| SW1 | SIM选择 | ESIM | 默认用贴片SIM卡，可选SIM卡座 |

### 2.5 L610引脚参考

| 功能 | L610模块引脚 | 说明 |
|------|------------|------|
| UART TXD | Pin 67 (TXD) | 模块发送，经电平转换后到J3-1为3.3V |
| UART RXD | Pin 68 (RXD) | 模块接收，经电平转换后到J3-2为3.3V |
| PWRKEY | Pin 21 (PWRKEY) | 拉低≥2s开机，拉低≥3.1s关机 |
| STATUS | Pin 61 (STATUS) | 1.8V输出，模块开机后高电平 |
| VBAT | Pin 57/58/59/60 | 3.8V供电 |
| VDD_EXT | Pin 7 | 1.8V输出（可取电给电平转换） |
| ANT_MAIN | Pin 49 | 4G主天线 |

---

## 3. L610 开机时序

### 3.1 ADP板供电即开机

ADP-L610-Arduino开发板的设计是 **上电即开机**（PWRKEY默认已处理），只需给ADP板USB供电，模块就会自动启动。

启动后验证步骤：
```
1. ADP板USB连PC
2. 打开SSCOM，选AT命令口（Port 0），115200
3. 发 AT\r          → 应收到 OK
4. 发 AT+CSQ\r      → 收到 +CSQ: xx,99  (xx为信号质量，0-31)
5. 发 AT+CGATT?\r   → 收到 +CGATT: 1    (1=已附着网络)
```

### 3.2 开机状态机（ESP32实现）

```
        ┌──────────────┐
        │  L610_OFF    │  ← 模块未上电 / 无法通信
        └──────┬───────┘
               │ 发送AT → 收到OK
               ▼
        ┌──────────────┐
        │  L610_READY  │  ← 可发送AT指令，模块正常
        └──────┬───────┘
               │ 连续3次AT超时
               ▼
        ┌──────────────┐
        │  L610_ERROR  │  ← 上报WS63
        └──────┬───────┘
               │ 尝试重新初始化
               ▼
        ┌──────────────┐
        │  L610_READY  │
        └──────────────┘
```

---

## 4. L610 MQTT AT指令集(已确认)

根据 `FIBOCOM L610 Series AT Commands_MQTT V1.0.2` 确认以下完整语法。

### 4.1 MQTT指令总览

| 指令 | 功能 | 使用场景 |
|------|------|---------|
| `AT+MQTTUSER` | 设置ClientID/用户名/密码 | 连接前必须设置 |
| `AT+MQTTOPEN` | 建立MQTT连接 | 连接MQTT Broker |
| `AT+MQTTPUB`  | 发布消息 | 上传数据到云端 |
| `AT+MQTTCLOSE` | 关闭MQTT连接 | 断开连接 |
| `AT+MQTTSUB` | 订阅Topic | 预留（下行控制） |
| `AT+MQTTBREAK` | URC意外断开通知 | 需在驱动中处理此URC |

### 4.2 指令详细语法

#### AT+MQTTUSER — 设置用户凭据

```
语法:  AT+MQTTUSER=<Client id>,<Username>,<Password>[,<ClientIDStr>]
响应:  OK
       或 +CME ERROR: <err>
查询:  AT+MQTTUSER?
       → +MQTTUSER: 1,"IekgXZSavYJ6KEJFyvb4","","WS63-AA:BB:CC:DD:EE:FF"
测试:  AT+MQTTUSER=?
       → +MQTTUSER: (1,2),(128),(128)[,(23)]
```

| 参数 | 说明 | 本项目使用值 |
|------|------|-------------|
| `<Client id>` | 整数 1 或 2（最多2个MQTT连接） | **1** |
| `<Username>` | 字符串，最长128字节 | **`IekgXZSavYJ6KEJFyvb4`** |
| `<Password>` | 字符串，最长128字节 | **`""`**（空） |
| `<ClientIDStr>` | 可选，最长23字节。不设置则用IMEI | **`"WS63-AA:BB:CC:DD:EE:FF"`** |

#### AT+MQTTOPEN — 建立MQTT连接

```
语法:  AT+MQTTOPEN=<Clientid>,<Remote IP/URL>,<RemotePort>,
                    <Cleansession flag>,<Keepalive time>[,<UseTls>]
响应:  OK
       （然后等待异步URC）
       +MQTTOPEN: 1,1   ← 连接成功
       或 +MQTTOPEN: 1,0   ← 连接失败
       或 +CME ERROR: <err>
查询:  AT+MQTTOPEN?
       → +MQTTOPEN: 1   ← Client 1 已打开
       → +MQTTOPEN: 0   ← 无打开的客户端
```

| 参数 | 说明 | 本项目使用值 |
|------|------|-------------|
| `<Clientid>` | 1 或 2 | **1** |
| `<Remote IP/URL>` | Broker地址，字符串 | **`"demo.thingskit.com"`** |
| `<RemotePort>` | 端口，1-65535 | **1883** |
| `<Cleansession flag>` | 0=不清除, 1=清除 | **1** |
| `<Keepalive time>` | 秒，1-300 | **60** |
| `<UseTls>` | 0=TCP, 2=TLS | **0**（初期不用TLS） |

> ⚠️ **注意：** `+MQTTOPEN` 响应是**异步**的。发送后立即返回 `OK`，过几秒后才收到 `+MQTTOPEN: 1,1`（成功）或 `1,0`（失败）。ESP32驱动需要等待这个URC。

#### AT+MQTTPUB — 发布消息

```
语法:  AT+MQTTPUB=<Client id>,<Topic>,<Qos>,<Retain flag>,<Payload>
响应:  OK
       （然后等待异步URC）
       +MQTTPUB: 1,1   ← 发布成功
       或 +MQTTPUB: 1,0   ← 发布失败
       或 ERROR
```

| 参数 | 说明 | 本项目使用值 |
|------|------|-------------|
| `<Client id>` | 1 或 2 | **1** |
| `<Topic>` | 字符串，1-255字节 | **`"device/WS63-AA:BB:CC:DD:EE:FF/up"`** |
| `<Qos>` | 0/1/2 | **1**（at least once） |
| `<Retain flag>` | 0/1 | **0**（不保留） |
| `<Payload>` | 字符串，0-1024字节 | 云端统一JSON |

> **异步返回值说明：** 同 `+MQTTOPEN`，`AT+MQTTPUB` 发送后先回OK，过一会才回 `+MQTTPUB: 1,1`。驱动需要处理这个URC。

#### AT+MQTTCLOSE — 关闭连接

```
语法:  AT+MQTTCLOSE=<Client id>
响应:  OK
       +MQTTCLOSE: 1,1   ← 关闭成功
       或 ERROR
```

#### +MQTTBREAK — URC意外断开通知（需驱动处理）

```
模块主动上报（不需要发送）：
+MQTTBREAK: 1,1   ← Client 1 因网络异常断开
+MQTTBREAK: 1,2   ← 无线链路断开
+MQTTBREAK: 1,3   ← GPRS网络未注册
```

### 4.3 完整连接流程（ESP32实现）

```c
// Step 1: 设置MQTT凭据
l610_at_send("AT+MQTTUSER=1,\"IekgXZSavYJ6KEJFyvb4\",\"\","
             "\"WS63-AA:BB:CC:DD:EE:FF\"\r");
// → OK

// Step 2: 建立MQTT连接
l610_at_send("AT+MQTTOPEN=1,\"demo.thingskit.com\",1883,1,60\r");
// → OK
// → 等待 +MQTTOPEN: 1,1（超时10秒）

// Step 3: 发布消息
l610_at_send("AT+MQTTPUB=1,\"device/WS63-AA:BB:CC:DD:EE:FF/up\","
             "1,0,\"{\\\"device_id\\\":\\\"WS63-AA:BB:CC:DD:EE:FF\\\","
             "\\\"msg_type\\\":\\\"task_done\\\"}\"\r");
// → OK
// → 等待 +MQTTPUB: 1,1（超时5秒）

// Step 4: 关闭连接
l610_at_send("AT+MQTTCLOSE=1\r");
// → OK
// → 等待 +MQTTCLOSE: 1,1
```

### 4.4 MQTT错误码

| 错误码 | 含义 |
|--------|------|
| 700 | 协议版本不支持 |
| 701 | ClientID被拒绝 |
| 702 | MQTT服务器不可用 |
| 703 | 用户名或密码错误 |
| 704 | 未授权 |

---

## 5. ESP32 新增软件模块

### 5.1 文件结构

```
main/modules/
  ├── 4g/                        ← 新增目录
  │   ├── l610_driver.c/h        ← L610硬件驱动 (UART AT收发 + URC解析)
  │   ├── l610_mqtt.c/h          ← L610 MQTT通道 (AT+MQTTUSER/OPEN/PUB/CLOSE封装)
  │   └── l610_manager.c/h       ← L610状态管理 (心跳/重连/错误上报)
  ├── system/
  │   ├── protocol_handler.c/h   ← [修改] 增加mqtt_publish/connect/disconnect等命令
  │   └── ... (不变)
  ├── camera/
  │   └── ... (不变)
  └── ai/
      └── ... (不变)
```

### 5.2 l610_driver（硬件驱动层）

职责：
- UART2初始化 (GPIO19=TX, GPIO20=RX, 115200, 8N1)
- AT指令发送（自动追加 `\r` 结尾）
- 同步应答解析（等OK/ERROR）
- 异步URC检测（`+MQTTOPEN:`、`+MQTTPUB:`、`+MQTTBREAK:`、`+MQTTMSG:`）
- 超时机制（默认5秒，可配置）
- 提供URC回调注册接口

接口：
```c
// 初始化UART2
esp_err_t l610_driver_init(uart_port_t uart_num, int tx_pin, int rx_pin);

// 发送AT指令并等待应答（同步阻塞）
// response_buf 会包含 "OK" 或 "ERROR" 或 "+XXX: ...\r\nOK"
esp_err_t l610_at_send(const char *cmd, char *response_buf,
                       size_t buf_size, TickType_t timeout_ms);

// 注册URC回调（异步通知：MQTT连接成功/失败、断开、收到消息等）
typedef void (*l610_urc_cb_t)(const char *urc_line);
void l610_register_urc_callback(l610_urc_cb_t callback);

// 检查模块是否就绪
bool l610_is_ready(void);

// 获取状态文本
const char *l610_status_str(void);
```

URC解析器需要识别的关键URC：

| URC模式 | 含义 | 驱动处理 |
|---------|------|---------|
| `+MQTTOPEN: 1,1` | MQTT连接成功 | 设置mqtt_state=CONNECTED |
| `+MQTTOPEN: 1,0` | MQTT连接失败 | 设置mqtt_state=DISCONNECTED |
| `+MQTTPUB: 1,1` | 发布成功 | 通知l610_mqtt层 |
| `+MQTTPUB: 1,0` | 发布失败 | 通知l610_mqtt层 |
| `+MQTTCLOSE: 1,1` | 关闭成功 | 通知l610_mqtt层 |
| `+MQTTBREAK: 1,<cause>` | 连接意外断开 | 触发自动重连 |
| `+MQTTMSG:` | 收到订阅消息 | 缓冲待读取（预留） |

### 5.3 l610_mqtt（MQTT通道层）

基于 `l610_driver`，封装L610的MQTT AT指令：

```c
// MQTT状态枚举
typedef enum {
    MQTT_DISCONNECTED = 0,
    MQTT_CONNECTING,
    MQTT_CONNECTED,
    MQTT_DISCONNECTING,
    MQTT_RECONNECTING,
    MQTT_ERROR
} l610_mqtt_state_t;

// 设置MQTT用户凭据
esp_err_t l610_mqtt_set_user(const char *client_id_str,
                              const char *username, const char *password);

// 连接MQTT Broker（AT+MQTTUSER + AT+MQTTOPEN）
// 注意：此函数会等AT响应OK，然后等待+MQTTOPEN URC最多timeout_sec秒
esp_err_t l610_mqtt_connect(const char *host, uint16_t port,
                             uint8_t clean_session, uint16_t keepalive,
                             int timeout_sec);

// 发布消息到Topic（AT+MQTTPUB）
esp_err_t l610_mqtt_publish(const char *topic, const char *payload,
                             uint8_t qos, uint8_t retain,
                             int timeout_sec);

// 断开MQTT连接（AT+MQTTCLOSE）
esp_err_t l610_mqtt_disconnect(int timeout_sec);

// 查询MQTT连接状态
l610_mqtt_state_t l610_mqtt_get_state(void);
```

### 5.4 l610_manager（状态管理层）

职责：
- L610模块初始化检测（发送AT验证响应）
- 注册URC回调 — 解析MQTT相关URC
- 心跳保活 — 每30秒检测L610状态
- MQTT自动重连 — 断线后重试（最多3次，间隔5秒）
- 错误上报给WS63

接口：
```c
// 初始化管理器
esp_err_t l610_manager_init(void);

// 启动心跳任务（FreeRTOS任务）
esp_err_t l610_manager_start(void);

// 停止心跳任务
esp_err_t l610_manager_stop(void);

// 获取状态（用于WS63查询）
esp_err_t l610_manager_get_status(l610_status_t *status);

// 心跳任务函数
void l610_heartbeat_task(void *arg);
```

### 5.5 protocol_handler 修改

在 `protocol_handler.c` 中增加：

```c
// 新增命令处理函数
static esp_err_t handle_mqtt_connect(cJSON *root);      // mqtt_connect
static esp_err_t handle_mqtt_disconnect(cJSON *root);   // mqtt_disconnect
static esp_err_t handle_mqtt_publish(cJSON *root);      // mqtt_publish
static esp_err_t handle_l610_status(cJSON *root);       // l610_status
static esp_err_t handle_l610_at(cJSON *root);           // l610_at (调试透传)
```

`cmd_dispatch_table` 增加映射：
```c
{"mqtt_connect",   handle_mqtt_connect},
{"mqtt_disconnect",handle_mqtt_disconnect},
{"mqtt_publish",   handle_mqtt_publish},
{"l610_status",    handle_l610_status},
{"l610_at",        handle_l610_at},
```

---

## 6. WS63 协议扩展

### 6.1 新增下行命令（WS63 → ESP32）

#### 6.1.1 MQTT连接管理

```json
// 连接MQTT Broker (4G通道)
{
  "cmd": "mqtt_connect",
  "host": "demo.thingskit.com",
  "port": 1883,
  "clean_session": 1,
  "keepalive": 60
}

// 断开MQTT连接
{"cmd":"mqtt_disconnect"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | `"mqtt_connect"` 或 `"mqtt_disconnect"` |
| `host` | string | 是(mqtt_connect) | MQTT Broker地址 |
| `port` | uint16 | 是(mqtt_connect) | MQTT Broker端口 |
| `clean_session` | uint8 | 否 | 0/1，默认1 |
| `keepalive` | uint16 | 否 | 心跳间隔(秒) |

> **MQTTUSER凭据由ESP32固件硬编码**（`IekgXZSavYJ6KEJFyvb4`/空密码/`WS63-{MAC}`），WS63不需要在每次连接时下发。

#### 6.1.2 MQTT发布（核心命令）

```json
{
  "cmd": "mqtt_publish",
  "topic": "device/WS63-AA:BB:CC:DD:EE:FF/up",
  "payload": "{json payload}",
  "qos": 1,
  "retain": 0
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"mqtt_publish"` |
| `topic` | string | 是 | MQTT主题，255字节以内 |
| `payload` | string | 是 | JSON字符串，**1024字节以内** |
| `qos` | uint8 | 否 | QoS等级，默认1 |
| `retain` | uint8 | 否 | retain标志，默认0 |

> ⚠️ **关键约束：** L610的`AT+MQTTPUB`的Payload最大1024字节（ASCII模式）。如果WS63组装的云端JSON超过此长度，需要使用HEX模式发送。或者压缩JSON字段名。

#### 6.1.3 L610状态查询

```json
{"cmd":"l610_status"}
```

#### 6.1.4 AT指令透传（调试用）

```json
{"cmd":"l610_at","at":"AT+CSQ"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cmd` | string | 是 | 固定值 `"l610_at"` |
| `at` | string | 是 | 要透传的AT指令（不含`\r`） |

### 6.2 新增上行消息（ESP32 → WS63）

#### 6.2.1 L610状态上报

```json
{
  "type": "l610_status",
  "mqtt_state": "connected",
  "signal_quality": 18
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"l610_status"` |
| `mqtt_state` | string | `"connected"` / `"disconnected"` / `"connecting"` / `"error"` |
| `signal_quality` | int | CSQ值(0-31)，99=未知 |

#### 6.2.2 MQTT发布结果

```json
{
  "type": "mqtt_publish_done",
  "result": "success",
  "topic": "device/WS63-AA:BB:CC:DD:EE:FF/up"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"mqtt_publish_done"` |
| `result` | string | `"success"` / `"failed"` |
| `topic` | string | 发布的Topic |

#### 6.2.3 MQTT连接状态变化

```json
{
  "type": "mqtt_connected",
  "state": "connected",
  "host": "demo.thingskit.com",
  "port": 1883
}
```

#### 6.2.4 L610错误通知

```json
{
  "type": "l610_error",
  "code": "L610_AT_TIMEOUT",
  "msg": "L610 not responding"
}
```

| 错误码 | 说明 |
|--------|------|
| `L610_AT_TIMEOUT` | AT指令超时 |
| `L610_MQTT_CONNECT_FAIL` | MQTT连接失败 |
| `L610_MQTT_PUBLISH_FAIL` | MQTT发布失败 |
| `L610_MQTT_LOST_CONNECTION` | MQTT连接意外断开（收到+MQTTBREAK） |
| `L610_NOT_RESPONDING` | L610模块无响应（连续3次AT超时） |

---

## 7. 数据流设计

### 7.1 场景A：WiFi通路（默认优先）

```
ESP32 完成拍摄+AI推理
    │
    ▼
ESP32 → UART1 → WS63
{"type":"task_done","task":"inventory","mac":"AA:BB:CC:DD:EE:FF",
 "result":"success","is_match":true,"weighted_confidence":0.892,...}
    │
    ▼
WS63 数据聚合层:
  ① 接收ESP32的UART上行消息
  ② 组装云端统一JSON格式:
     {
       "device_id": "WS63-AA:BB:CC:DD:EE:FF",
       "timestamp": 1712345678,
       "msg_type": "task_done",
       "task": "inventory",
       "mac": "AA:BB:CC:DD:EE:FF",
       "result": "success",
       "is_match": true,
       "item_name": "扳手",
       "storage_area": "A",
       "quantity": 50,
       "upload_channel": "wifi"
     }
  ③ 判断WiFi状态 → 正常
    ▼
WS63 MQTT直连 ThingsKit → publish to device/WS63-AA:BB:CC:DD:EE:FF/up
```

### 7.2 场景B：4G通路（WiFi不可用）

```
ESP32 完成拍摄+AI推理
    │
    ▼
ESP32 → UART1 → WS63
{"type":"task_done","task":"inventory","mac":"AA:BB:CC:DD:EE:FF",
 "result":"success","is_match":true,"weighted_confidence":0.892,...}
    │
    ▼
WS63 数据聚合层: 同①~②（upload_channel: "4g"）
  ③ 判断WiFi状态 → 不可用
    │
    ▼
WS63 通过UART1下发4G上传指令:
{
  "cmd": "mqtt_publish",
  "topic": "device/WS63-AA:BB:CC:DD:EE:FF/up",
  "payload": "{同上的云端统一JSON, 仅upload_channel改为\"4g\"}",
  "qos": 1
}
    │
    ▼
ESP32 收到 mqtt_publish:
  ① 提取 topic 和 payload
  ② 通过UART2转发给L610:
     AT+MQTTPUB=1,"device/WS63-.../up",1,0,"{...}"\r
    │
    ▼
L610 → 4G网络 → ThingsKit MQTT Broker
    │
    ▼
L610 返回 OK → 异步 URC: +MQTTPUB: 1,1
    │
    ▼
ESP32 通过UART1回传结果给WS63:
{"type":"mqtt_publish_done","result":"success","topic":"device/WS63-AA:BB:CC:DD:EE:FF/up"}
```

### 7.3 云端统一JSON格式

```json
{
  "device_id": "WS63-AA:BB:CC:DD:EE:FF",
  "timestamp": 1712345678,
  "msg_type": "task_done",
  "task": "inventory",
  "mac": "AA:BB:CC:DD:EE:FF",
  "result": "success",
  "is_match": true,
  "weighted_confidence": 0.892,
  "front_confidence": 0.91,
  "side_confidence": 0.88,
  "top_confidence": 0.85,
  "threshold": 0.75,
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50,
  "upload_channel": "4g"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `device_id` | string | 是 | WS63设备标识 |
| `timestamp` | int64 | 是 | Unix时间戳（秒） |
| `msg_type` | string | 是 | 消息类型: `"task_done"` / `"error"` / `"status"` / `"pong"` |
| `task` | string | 否 | 任务类型: `"register"` / `"inventory"` / `"outbound"` / `"delete"` |
| `mac` | string | 否 | 资产MAC地址 |
| `result` | string | 否 | `"success"` / `"failed"` / `"cancelled"` |
| `is_match` | bool | 否 | 盘点/出库是否匹配 |
| `item_name` | string | 否 | 物品名称 |
| `storage_area` | string | 否 | 存放区域 |
| `quantity` | uint32 |