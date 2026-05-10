# L610 4G模块 AT 指令参考文档

> 本文档记录 ADP-L610 模块支持的 AT 指令及其在项目中的使用方法。
> 适用于 L610 系列 4G 通信模块（基于移远 EC200U 系列）。

---

## 目录

1. [基础 AT 指令](#1-基础-at-指令)
2. [MQTT AT 指令](#2-mqtt-at-指令)
3. [网络相关指令](#3-网络相关指令)
4. [模块信息查询](#4-模块信息查询)
5. [AT 指令透传（调试）](#5-at-指令透传调试)
6. [常见错误码](#6-常见错误码)
7. [故障排除](#7-故障排除)

---

## 1. 基础 AT 指令

| 指令 | 预期响应 | 说明 |
|------|---------|------|
| `AT` | `OK` | 测试模块是否响应 |
| `AT+CPIN?` | `+CPIN: READY` | 查询 SIM 卡状态 |
| `AT+CSQ` | `+CSQ: 18,99` | 查询信号质量（0-31，99=未知） |
| `AT+COPS?` | `+COPS: 0,0,"CHINA MOBILE",7` | 查询当前运营商 |
| `AT+CREG?` | `+CREG: 0,1` | 查询网络注册状态（1=已注册） |
| `AT+CGATT?` | `+CGATT: 1` | 查询 GPRS 附着状态（1=已附着） |
| `AT+CGDCONT?` | `+CGDCONT: 1,"IP","CMNET"` | 查询 PDP 上下文 |
| `AT+IPR=115200` | `OK` | 设置串口波特率 |
| `AT&W` | `OK` | 保存当前配置到 NVRAM |
| `AT+CFUN=1` | `OK` | 设置模块全功能模式 |
| `AT+CFUN=0` | `OK` | 设置模块最小功能模式（飞行模式） |
| `AT+QPOWD` | `POWERED DOWN` | 模块关机 |

---

## 2. MQTT AT 指令

### 2.1 设置 MQTT 用户凭据

```
AT+MQTTUSER=1,"client_id","username","password"
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `mode` | int | 必须为 `1`（设置模式） |
| `client_id` | string | 客户端标识，如 `"WS63-AA:BB:CC:DD:EE:FF"` |
| `username` | string | MQTT 用户名 |
| `password` | string | MQTT 密码 |

**响应示例：**
```
OK
```

### 2.2 建立 MQTT 连接

```
AT+MQTTOPEN=1,"host",port,clean_session,keepalive
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `mode` | int | 必须为 `1` |
| `host` | string | MQTT Broker 地址，如 `"demo.thingskit.com"` |
| `port` | int | MQTT Broker 端口，如 `1883` |
| `clean_session` | int | `0`=不清理, `1`=清理（默认） |
| `keepalive` | int | 心跳间隔（秒），默认 60 |

**成功响应：**
```
OK
+MQTTOPEN: 1,1
```

> **注意：** `+MQTTOPEN: 1,1` 是异步 URC，可能在 `OK` 之后几秒才到达。
> 项目中使用 `l610_mqtt_connect()` 会自动等待此 URC。

### 2.3 发布 MQTT 消息

```
AT+MQTTPUB=1,"topic",qos,retain,"payload"
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `mode` | int | 必须为 `1` |
| `topic` | string | MQTT 主题，最长 255 字节 |
| `qos` | int | QoS 等级：`0` / `1` / `2` |
| `retain` | int | retain 标志：`0` / `1` |
| `payload` | string | JSON 字符串，最长 **1024 字节** |

**成功响应：**
```
OK
+MQTTPUB: 1,1
```

> **注意：** payload 中的双引号需转义为 `\"`，实际项目中的 `l610_mqtt_publish()` 会处理转义。

### 2.4 关闭 MQTT 连接

```
AT+MQTTCLOSE=1
```

**响应示例：**
```
OK
+MQTTCLOSE: 1,1
```

### 2.5 意外断开通知（URC）

```
+MQTTBREAK: 1,0
```

> 收到此 URC 表示 MQTT 连接意外断开，驱动层需自动处理重连。
> 项目中的 `l610_mqtt_urc_handler()` 会捕获此 URC 并更新状态为 `MQTT_STATE_ERROR`。

---

## 3. 网络相关指令

### 3.1 查询网络注册状态

```
AT+CREG?
```

**响应：**
```
+CREG: 0,1
```
- 第二个值含义：`0`=未注册, `1`=已注册, `2`=搜索中, `3`=注册被拒, `5`=已注册（漫游）

### 3.2 查询 GPRS 附着状态

```
AT+CGATT?
```

**响应：**
```
+CGATT: 1
```
- `0`=未附着, `1`=已附着

### 3.3 手动触发 GPRS 附着

```
AT+CGATT=1
```

### 3.4 查询信号质量

```
AT+CSQ
```

**响应：**
```
+CSQ: 18,99
```
- 第一个值：信号强度（0-31，值越大信号越强）
- 第二个值：误码率（0-7, 99=未知）
- 信号强度参考：`0`=极差, `10`=差, `15`=一般, `20`=良好, `25`=优秀, `31`=极好

### 3.5 查询模块工作模式

```
AT+CFUN?
```

**响应：**
```
+CFUN: 1
```
- `0`=最小功能（飞行模式）, `1`=全功能

---

## 4. 模块信息查询

| 指令 | 响应示例 | 说明 |
|------|---------|------|
| `AT+GSN` | `868120000000000` | 查询 IMEI 号 |
| `AT+CIMI` | `460011234567890` | 查询 IMSI 号 |
| `AT+ICCID` | `8986112233445566778` | 查询 SIM 卡 ICCID |
| `AT+CGMR` | `EC200UCGMR02A01M08` | 查询固件版本 |
| `AT+CGMM` | `EC200U` | 查询模块型号 |
| `AT+CGMI` | `Quectel` | 查询制造商 |
| `AT+CCLK?` | `+CCLK: "24/10/15,15:30:00+32"` | 查询 RTC 时间 |

---

## 5. AT 指令透传（调试）

项目支持通过 WS63 协议中的 `l610_at` 命令进行 AT 指令透传调试：

### 5.1 发送 AT 指令

```json
{"cmd":"l610_at","at":"AT+CSQ"}
```

### 5.2 成功响应

```json
{
  "type": "l610_at_result",
  "cmd": "AT+CSQ",
  "result": "ok",
  "response": "+CSQ: 18,99\r\nOK"
}
```

### 5.3 超时响应（模块未响应）

```json
{
  "type": "l610_error",
  "code": "L610_AT_TIMEOUT",
  "msg": "L610 not responding"
}
```

### 5.4 常用调试步骤

当需要排查 4G 模块问题时，按以下顺序发 AT 指令：

```
1. AT                  → 检测模块是否存活
2. AT+CPIN?            → 检测 SIM 卡是否插入且正常
3. AT+CSQ              → 检查信号强度（≥10 为可用）
4. AT+CREG?            → 检查网络注册（值为 1 或 5 为已注册）
5. AT+CGATT?           → 检查 GPRS 附着（值为 1 为成功）
6. AT+MQTTUSER=...     → 设置 MQTT 凭据
7. AT+MQTTOPEN=...     → 连接 MQTT Broker
8. AT+MQTTPUB=...      → 发布测试消息
```

---

## 6. 常见错误码

| 错误码 | 含义 | 可能原因 |
|--------|------|---------|
| `L610_AT_TIMEOUT` | AT 指令超时 | 模块未供电、串口连接异常、波特率不匹配 |
| `L610_MQTT_CONNECT_FAIL` | MQTT 连接失败 | Broker 地址错误、SIM 卡无网络、防火墙阻挡 |
| `L610_MQTT_PUBLISH_FAIL` | MQTT 发布失败 | Payload 过长、Topic 非法、MQTT 连接已断开 |
| `L610_MQTT_LOST_CONNECTION` | MQTT 连接意外断开 | 网络信号中断、Broker 重启 |
| `L610_NOT_RESPONDING` | 模块连续 3 次无响应 | 模块死机、电源不足、硬件故障 |

---

## 7. 故障排除

### 7.1 模块不响应 AT

1. 检查模块供电（ADP-L610 需 5V 供电，J3 排针 5V/3.3V 电平）
2. 检查串口连接：ESP32-S3 GPIO19(TX) ↔ L610 RX, GPIO20(RX) ↔ L610 TX
3. 检查波特率是否匹配（默认 115200）
4. 发送 `AT` 测试，若无响应检查复位引脚

### 7.2 MQTT 连接失败

1. 确认 SIM 卡已插入且有信号：`AT+CSQ` ≥ 10
2. 确认已注册网络：`AT+CREG?` 返回 `0,1` 或 `0,5`
3. 确认已 GPRS 附着：`AT+CGATT?` 返回 `1`
4. 确认 Broker 地址和端口可正确解析
5. 检查 MQTT 用户名密码配置是否正确

### 7.3 MQTT 发布失败

1. 确认 payload 不超过 1024 字节
2. 确认 Topic 不超过 255 字节
3. 确认 MQTT 连接状态为已连接：`{"cmd":"l610_mqtt_check"}`
4. 检查 QoS 等级是否被 Broker 支持

### 7.4 信号质量差

1. 检查 4G 天线连接是否牢固
2. 将天线置于开阔位置
3. 确认工作频段是否匹配本地运营商
4. 考虑使用信号放大器