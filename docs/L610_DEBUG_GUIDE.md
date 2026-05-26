# L610 4G模块调试指南 v3.1

> **文档版本**: v3.1  
> **最后更新**: 2026-05-10  
> **适用项目**: CAM_AI (ESP32-S3 + L610 4G模块)  

---

## 目录

1. [环境准备](#1-环境准备)
2. [基础功能验证](#2-基础功能验证)
3. [MQTT业务测试](#3-mqtt业务测试)
4. [异常场景测试](#4-异常场景测试)
5. [性能压力测试](#5-性能压力测试)
6. [故障排查](#6-故障排查)
7. [附录](#7-附录)

---

## 1. 环境准备

### 1.1 硬件连接检查

**L610模块接线**:
```
ESP32-S3          L610 Module
────────          ───────────
GPIO4 (UART2_TX) → RX
GPIO5 (UART2_RX) ← TX
GND              ↔ GND
3.3V/5V          → VCC (根据模块规格)
```

**检查清单**:
- ✅ UART接线正确（TX↔RX交叉）
- ✅ GND共地
- ✅ 供电电压符合模块要求
- ✅ SIM卡已插入且激活
- ✅ 天线已连接

### 1.2 软件配置

**menuconfig设置**:
```bash
idf.py menuconfig
```

确认以下配置：
```
Component config → ESP-DL → Enable ESP-DL: [*]
Component config → ESP32 Camera → Camera Model: ESP32-S3-CAM
Serial flasher config → Default serial port: COMx (Windows) / /dev/ttyUSBx (Linux)
```

### 1.3 编译烧录

```bash
# 清理并重新编译
idf.py fullclean
idf.py build

# 烧录并监控
idf.py -p COMx flash monitor
```

**预期启动日志**:
```
I (xxx) l610_driver: L610 driver initialized (UART2, TX=4, RX=5)
I (xxx) l610_manager: L610 manager started
I (xxx) protocol_handler: L610 manager send callback registered
I (xxx) main: System initialization complete
```

---

## 2. 基础功能验证

### 2.1 L610模块通信测试

**测试命令** (通过WS63串口发送):
``json
{"cmd":"l610_at","at":"AT"}
```

**预期响应**:
```
{
  "type": "l610_at_result",
  "cmd": "AT",
  "result": "ok",
  "response": "OK"
}
```

**失败排查**:
- 无响应 → 检查UART接线和供电
- 返回ERROR → 检查波特率配置（应为115200）

### 2.2 信号质量查询

**命令**:
``json
{"cmd":"l610_at","at":"AT+CSQ"}
```

**预期响应**:
```
{
  "type": "l610_at_result",
  "cmd": "AT+CSQ",
  "result": "ok",
  "response": "+CSQ: 25,99\r\nOK"
}
```

**判断标准**:
- CSQ > 20: 信号优秀
- CSQ 10-20: 信号良好
- CSQ < 10: 信号弱，可能影响稳定性

### 2.3 网络附着状态

**命令**:
``json
{"cmd":"l610_at","at":"AT+CGATT?"}
```

**预期响应**:
```
{
  "type": "l610_at_result",
  "cmd": "AT+CGATT?",
  "result": "ok",
  "response": "+CGATT: 1\r\nOK"
}
```

**判断标准**:
- `+CGATT: 1` → 已附着GPRS网络 ✅
- `+CGATT: 0` → 未附着，检查SIM卡和网络覆盖 ❌

### 2.4 AT指令重试机制验证

**测试方法**:
在网络不稳定时执行AT指令，观察日志中的重试信息。

**预期日志**:
```
D (xxx) l610_driver: AT timeout (retry 1/2): AT+CSQ
I (xxx) l610_driver: AT command succeeded after 1 retries: AT+CSQ
```

**验证要点**:
- ✅ 最多重试3次（L610_AT_MAX_RETRY）
- ✅ 重试间隔200ms
- ✅ 成功后重置连续超时计数

---

## 3. MQTT业务测试

### 3.1 连接MQTT服务器

**前置条件**:
- L610模块已初始化
- 网络附着成功（CGATT=1）
- 信号质量良好（CSQ>10）

**命令**:
```json
{
  "cmd": "mqtt_connect",
  "host": "mqtt.thingskit.com",
  "port": 1883,
  "clean_session": 1,
  "keepalive": 60
}
```

**验证步骤**:

**步骤1**: 查看ClientID生成日志
```
I (xxx) protocol_handler: L610 ClientID generated: WS63-AA:BB:CC:DD:EE:FF
I (xxx) l610_mqtt: MQTT user set: IekgXZSavYJ6KEJFyvb4, ClientID=WS63-AA:BB:CC:DD:EE:FF
```

**步骤2**: 等待连接成功响应
```json
{
  "type": "mqtt_connected",
  "state": "connected",
  "host": "mqtt.thingskit.com",
  "port": 1883
}
```

**步骤3**: 云端验证
1. 登录ThingsKit平台
2. 查看设备列表
3. 确认设备状态为"在线"
4. 检查ClientID是否为`WS63-{MAC}`格式

**常见问题**:
- 连接超时 → 检查网络和MQTT服务器地址
- 认证失败 → 检查username/password配置
- ClientID冲突 → 确保每个设备MAC唯一

### 3.2 发布消息测试

**基本测试**:
```json
{
  "cmd": "mqtt_publish",
  "topic": "test/topic",
  "payload": "Hello from ESP32-S3",
  "qos": 1,
  "retain": 0
}
```

**验证步骤**:
1. 查看发送日志：
```
I (xxx) l610_mqtt: Publishing to test/topic (qos=1, retain=0)
I (xxx) l610_mqtt: MQTT publish URC received: +MQTTPUB: 1,0
```

2. 云端订阅`test/topic`主题，确认收到消息

3. 检查消息内容是否正确

### 3.3 Payload长度边界测试

**测试超长Payload**:
```json
{
  "cmd": "mqtt_publish",
  "topic": "test/topic",
  "payload": "<生成1025字节数据>",
  "qos": 1,
  "retain": 0
}
```

**Python脚本生成测试数据**:
```
import json

payload = "A" * 1025  # 1025字节
cmd = {
    "cmd": "mqtt_publish",
    "topic": "test/topic",
    "payload": payload,
    "qos": 1,
    "retain": 0
}
print(json.dumps(cmd))
```

**预期响应**:
```
{
  "type": "error",
  "code": "ERR_INVALID_SIZE",
  "msg": "Payload too long: 1025 bytes (max 1024)"
}
```

**日志验证**:
```
E (xxx) l610_mqtt: Payload too long: 1025 bytes (max 1024)
```

### 3.4 断开连接测试

**命令**:
```json
{"cmd": "mqtt_disconnect"}
```

**预期响应**:
```json
{
  "type": "mqtt_connected",
  "state": "disconnected",
  "host": "mqtt.thingskit.com",
  "port": 1883
}
```

**验证要点**:
- ✅ 云端设备状态变为"离线"
- ✅ L610内部状态变为DISCONNECTED
- ✅ 资源正确释放（无内存泄漏）

---

## 4. 异常场景测试

### 4.1 MQTT意外断开上报

**测试目的**: 验证L610主动上报机制

**测试步骤**:
1. 正常连接MQTT
2. 手动关闭路由器或拔掉网线
3. 等待30秒（心跳检测间隔）
4. 观察WS63是否收到错误通知

**预期响应**:
```
{
  "type": "l610_error",
  "code": "MQTT_DISCONNECTED",
  "msg": "MQTT connection lost"
}
```

**日志验证**:
```
W (xxx) l610_manager: MQTT disconnected (URC received)
I (xxx) protocol_handler: L610 error reported to WS63
```

### 4.2 模块失联检测

**测试目的**: 验证连续超时判定机制

**测试步骤**:
1. 拔掉L610模块TX线（模拟通信中断）
2. 等待心跳检测（每30秒一次）
3. 观察连续3次超时后的日志

**预期日志**:
```
W (xxx) l610_driver: AT timeout after 5000 ms (consecutive=1): AT+CSQ
W (xxx) l610_driver: AT timeout after 5000 ms (consecutive=2): AT+CSQ
W (xxx) l610_driver: AT timeout after 5000 ms (consecutive=3/3, LOST): AT+CSQ
E (xxx) l610_manager: L610 module lost! Consecutive timeouts: 3
```

**预期上报**:
```
{
  "type": "l610_error",
  "code": "L610_NOT_RESPONDING",
  "msg": "L610 module not responding"
}
```

### 4.3 重连恢复测试

**测试目的**: 验证自动重连功能

**测试步骤**:
1. 断开网络连接
2. 等待模块标记为LOST
3. 恢复网络连接
4. 观察是否自动重连

**预期行为**:
- 网络恢复后，心跳任务检测到模块就绪
- 自动重新连接MQTT
- 上报`mqtt_connected`事件

---

## 5. 性能压力测试

### 5.1 连续发布测试

**测试脚本** (Python):
```
import json
import serial
import time

ser = serial.Serial('COMx', 115200, timeout=1)

def send_command(cmd_dict):
    cmd_str = json.dumps(cmd_dict) + '\n'
    ser.write(cmd_str.encode())
    time.sleep(0.1)  # 等待发送
    response = ser.readline().decode().strip()
    return response

# 连续发送100条消息
success_count = 0
for i in range(100):
    cmd = {
        "cmd": "mqtt_publish",
        "topic": "stress/test",
        "payload": f"Message #{i}",
        "qos": 1,
        "retain": 0
    }
    
    try:
        resp = send_command(cmd)
        if '"result":"ok"' in resp or '"type":"mqtt_publish_result"' in resp:
            success_count += 1
            print(f"[{i+1}/100] Success")
        else:
            print(f"[{i+1}/100] Failed: {resp}")
    except Exception as e:
        print(f"[{i+1}/100] Error: {e}")
    
    time.sleep(0.5)  # 间隔500ms

print(f"\nStress test completed!")
print(f"Success rate: {success_count}/100 ({success_count}%)")
```

**通过标准**:
- ✅ 成功率 ≥ 95%
- ✅ 平均响应时间 < 2秒
- ✅ 无内存泄漏（heap free稳定）

**监控heap**:
```json
{"cmd":"sys_info"}
```
观察`heap_free`字段在测试前后的变化。

### 5.2 重连稳定性测试

**测试脚本**:
```
import json
import serial
import time

ser = serial.Serial('COMx', 115200, timeout=5)

def send_command(cmd_dict):
    cmd_str = json.dumps(cmd_dict) + '\n'
    ser.write(cmd_str.encode())
    time.sleep(0.5)
    # 读取多行响应
    responses = []
    for _ in range(10):
        line = ser.readline().decode().strip()
        if line:
            responses.append(line)
    return responses

# 重连10次
for i in range(10):
    print(f"\n=== Reconnect test #{i+1}/10 ===")
    
    # 断开
    disconnect_cmd = {"cmd": "mqtt_disconnect"}
    send_command(disconnect_cmd)
    time.sleep(2)
    
    # 重连
    connect_cmd = {
        "cmd": "mqtt_connect",
        "host": "mqtt.thingskit.com",
        "port": 1883
    }
    responses = send_command(connect_cmd)
    
    # 检查是否成功
    success = any('"state":"connected"' in r for r in responses)
    print(f"Result: {'SUCCESS' if success else 'FAILED'}")
    
    if not success:
        print("Responses:", responses)
        break
    
    time.sleep(3)

print("\nReconnect stability test completed!")
```

**通过标准**:
- ✅ 10次重连全部成功
- ✅ 每次重连时间 < 15秒
- ✅ 无资源泄漏

---

## 6. 故障排查

### 6.1 L610无响应

**症状**: 
- 所有AT指令超时
- 日志显示`AT timeout`

**排查步骤**:

1. **检查UART接线**
   ```
   ESP32 GPIO4 (TX) → L610 RX
   ESP32 GPIO5 (RX) ← L610 TX
   ```
   ⚠️ 注意：TX和RX必须交叉连接！

2. **测量供电电压**
   ```bash
   万用表测量VCC和GND之间电压
   应为3.3V或5V（根据模块规格）
   ```

3. **检查SIM卡**
   - 确认SIM卡已插入
   - 确认SIM卡已激活且有流量
   - 尝试更换SIM卡

4. **直接测试L610**
   ```
   使用USB转TTL模块直接连接L610
   打开AT助手软件（如SSCOM）
   发送"AT"，应返回"OK"
   ```

5. **检查波特率**
   ```c
   // l610_config.h中确认
   #define L610_UART_BAUD 115200
   ```

### 6.2 MQTT连接失败

**症状**: 
- `mqtt_connect`返回ERROR
- 日志显示`MQTT open failed`

**排查步骤**:

1. **检查网络连接**
   ```json
   {"cmd":"l610_at","at":"AT+CGATT?"}
   ```
   应返回`+CGATT: 1`

2. **检查信号质量**
   ```json
   {"cmd":"l610_at","at":"AT+CSQ"}
   ```
   CSQ应 > 10

3. **验证MQTT参数**
   ```c
   // l610_config.h
   #define L610_MQTT_USERNAME "your_username"
   #define L610_MQTT_PASSWORD "your_password"
   ```

4. **开启L610详细日志**
   ```json
   {"cmd":"l610_at","at":"AT+MQTTLOG=1"}
   ```
   查看具体错误原因

5. **测试MQTT服务器可达性**
   ```
   在电脑上使用MQTT客户端测试相同参数
   确认服务器地址和端口正确
   ```

### 6.3 Payload发送失败

**症状**: 
- `mqtt_publish`返回ERR_INVALID_SIZE

**原因**: 
- Payload超过1024字节限制

**解决方案**:

1. **压缩JSON数据**
   ```python
   # 使用紧凑格式
   json.dumps(data, separators=(',', ':'))
   ```

2. **分片发送**
   ```python
   # 将大数据分成多个消息
   chunk_size = 1000
   for i in range(0, len(data), chunk_size):
       chunk = data[i:i+chunk_size]
       publish(topic, chunk)
   ```

3. **使用二进制格式**
   ```
   考虑使用Protocol Buffers或MessagePack
   代替JSON以减少体积
   ```

### 6.4 内存泄漏

**症状**: 
- `heap_free`持续减小
- 长时间运行后系统崩溃

**排查步骤**:

1. **监控heap变化**
   ```json
   {"cmd":"sys_info"}
   ```
   记录`heap_free`值，每隔5分钟检查一次

2. **调用清理函数**
   ```json
   {"cmd":"l610_disconnect"}
   ```
   观察heap是否恢复

3. **启用heap跟踪**
   ```bash
   idf.py menuconfig
   → Component config → ESP System Settings
   → Enable heap task tracking: [*]
   ```

4. **检查cJSON对象释放**
   ```c
   // 确保所有cJSON_CreateObject都有对应的cJSON_Delete
   cJSON *obj = cJSON_CreateObject();
   // ... 使用 ...
   cJSON_Delete(obj);  // ✅ 必须释放
   ```

### 6.5 频繁重连

**症状**: 
- MQTT连接频繁断开重连
- 日志显示大量`MQTT_DISCONNECTED`

**可能原因**:
1. Keepalive时间过短
2. 网络不稳定
3. 服务器负载过高

**解决方案**:

1. **增加Keepalive时间**
   ```json
   {
     "cmd": "mqtt_connect",
     "host": "mqtt.thingskit.com",
     "port": 1883,
     "keepalive": 120  // 从60增加到120秒
   }
   ```

2. **优化网络环境**
   - 改善天线位置
   - 避开干扰源
   - 使用信号放大器

3. **联系服务器管理员**
   - 检查服务器负载
   - 确认最大连接数限制

---

## 7. 附录

### 7.1 常用AT指令速查

| 指令 | 说明 | 示例响应 |
|------|------|----------|
| `AT` | 测试通信 | `OK` |
| `AT+CSQ` | 信号质量 | `+CSQ: 25,99` |
| `AT+CGATT?` | 网络附着 | `+CGATT: 1` |
| `AT+CREG?` | 注册状态 | `+CREG: 0,1` |
| `AT+CGDCONT?` | PDP上下文 | `+CGDCONT: 1,"IP","cmnet"` |
| `AT+MQTTUSER` | 设置MQTT用户 | `OK` |
| `AT+MQTTOPEN` | 打开MQTT连接 | `+MQTTOPEN: 1,0` |
| `AT+MQTTPUB` | 发布消息 | `+MQTTPUB: 1,0` |
| `AT+MQTTCLOSE` | 关闭MQTT连接 | `OK` |
| `AT+MQTTLOG` | MQTT日志开关 | `OK` |

### 7.2 错误码对照表

| 错误码 | 说明 | 解决方案 |
|--------|------|----------|
| `ERR_INVALID_ARG` | 参数错误 | 检查JSON字段是否完整 |
| `ERR_INVALID_SIZE` | 长度超限 | Payload不超过1024字节 |
| `ERR_TIMEOUT` | 超时 | 检查网络和模块状态 |
| `L610_NOT_RESPONDING` | 模块失联 | 检查UART接线和供电 |
| `MQTT_DISCONNECTED` | MQTT断开 | 检查网络和服务器状态 |
| `MQTT_CONNECT_FAILED` | 连接失败 | 验证MQTT参数和认证信息 |

### 7.3 日志级别说明

| 级别 | 宏 | 说明 |
|------|-----|------|
| ERROR | `ESP_LOGE` | 严重错误，需要立即处理 |
| WARNING | `ESP_LOGW` | 警告，可能影响稳定性 |
| INFO | `ESP_LOGI` | 重要信息，关键流程节点 |
| DEBUG | `ESP_LOGD` | 调试信息，详细执行过程 |
| VERBOSE | `ESP_LOGV` | 详细信息，所有细节 |

**修改日志级别**:
```bash
idf.py menuconfig
→ Component config → Log output
→ Default log verbosity: Debug
```

### 7.4 性能指标参考

| 指标 | 正常范围 | 说明 |
|------|---------|------|
| CSQ信号质量 | > 15 | 越大越好 |
| AT指令响应时间 | < 1秒 | 网络良好时 |
| MQTT连接时间 | < 15秒 | 包含网络注册 |
| MQTT发布延迟 | < 2秒 | QoS=1时 |
| Heap空闲内存 | > 50KB | 避免碎片化 |
| 心跳间隔 | 30秒 | 可配置 |

---

## 快速测试清单

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

---

**文档版本**: v3.3  
**更新日期**: 2026-05-26  
**维护者**: TcXc  
**反馈邮箱**: 202500201056@stumail.sztu.edu.cn
