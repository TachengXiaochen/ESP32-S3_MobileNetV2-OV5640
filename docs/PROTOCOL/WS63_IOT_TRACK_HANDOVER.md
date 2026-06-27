# WS63 对接文档 — 物联网大赛赛道（v2.2）

> **版本**: v2.2 | **日期**: 2026-06-26  
> **架构**: **WS63 主控**，ESP32 是摄像头 + L610 AT 透传  
> **MQTT Broker**: **EMQX Cloud** `mqtts://h13f6185.ala.cn-hangzhou.emqxsl.cn:8883`  
> **上云主题**: **`v1/gateway/telemetry`**（方案 A，单主题）  
> **前置阅读**: `docs/plan/web-dashboard-plan.md` §1.4–§1.5

---

## 一、架构

```
BS21E → WS63(SLE汇总) → UART1 mqtt_publish → ESP32(透传) → L610 4G → EMQX Cloud
```

**核心**：WS63 通过 UART1 发 `mqtt_connect` / `mqtt_publish` 给 ESP32，ESP32 不解析业务 JSON，直接透传 L610 AT 指令。

---

## 二、上云 Payload — 方案 A（已选定）

**不再使用** `nelink/tag/telemetry` 与 `nelink/esp32/status` 双主题。

所有数据合并为 **ThingsKit 网关格式**，单次发布到 **`v1/gateway/telemetry`**：

```json
{
  "tag_001": [{"tag_id":1,"zone":"A1","item":"Type-C","qty":50,"status":2,"battery":95}],
  "gateway": [{"camera_ready":true,"storage_ready":true,"free_heap":185632,"storage_total_mb":8,"storage_free_mb":6,"state":"idle"}]
}
```

| Key | 来源 | 触发 |
|-----|------|------|
| `tag_XXX` | BS21E → WS63 SLE | 标签广播/盘点变更时 |
| `gateway` | WS63 `sys_info` 查 ESP32 | 每 30s（IDLE）+ 标签发布时附带缓存 |

---

## 三、WS63 改动清单

### 3.1 `biz_tag_map.c` — 转发到 ESP32（合并 gateway）

标签变更时构建网关 JSON（含变更 tag + 缓存 gateway），包装为 `mqtt_publish`：

```c
void biz_forward_tag_to_esp32(biz_tag_entry_t *entry)
{
    if (entry == NULL) return;

    cJSON *payload = cJSON_CreateObject();

    /* tag 子设备 */
    cJSON *arr = cJSON_CreateArray();
    cJSON *tag = cJSON_CreateObject();
    cJSON_AddNumberToObject(tag, "tag_id", entry->tag_id);
    cJSON_AddStringToObject(tag, "zone", entry->zone);
    cJSON_AddStringToObject(tag, "item", entry->item);
    cJSON_AddNumberToObject(tag, "qty", entry->qty);
    cJSON_AddNumberToObject(tag, "status", entry->status);
    cJSON_AddNumberToObject(tag, "battery", entry->battery);
    cJSON_AddItemToArray(arr, tag);
    char key[16];
    snprintf(key, sizeof(key), "tag_%03u", (unsigned int)entry->tag_id);
    cJSON_AddItemToObject(payload, key, arr);

    /* gateway 子设备（从本地缓存合并，见 §3.3） */
    biz_gateway_append_to_payload(payload);

    char *payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapper, "cmd", "mqtt_publish");
    cJSON_AddStringToObject(wrapper, "topic", "v1/gateway/telemetry");
    cJSON_AddStringToObject(wrapper, "payload", payload_str);
    cJSON_free(payload_str);

    char *str = cJSON_PrintUnformatted(wrapper);
    cJSON_Delete(wrapper);
    if (str) { biz_raw_json_send(str); cJSON_free(str); }
}
```

### 3.2 `biz_sle.c` — 条件编译

每处 `biz_publish_tag_update(entry)` 改为：

```c
#if WS63_CLOUD_MODE == CLOUD_MODE_ESP32_L610
    biz_forward_tag_to_esp32(entry);
#else
    biz_publish_tag_update(entry);
#endif
```

### 3.3 定时 30s — sys_info 合并进 gateway（非独立 topic）

```
WS63 (每 30s, IDLE)
  → UART1: {"cmd":"sys_info"}
  → ESP32: {"type":"sys_info","camera_ready":true,"storage_ready":true,
             "free_heap":185632,"storage_total_mb":8,"storage_free_mb":6,"state":"idle"}
  → WS63 更新 gateway 缓存
  → 合并所有 tag_XXX + gateway → mqtt_publish → v1/gateway/telemetry
```

> **注意**：不要单独 publish 到 `nelink/esp32/status`。单次 payload ≤ **1024** 字节；ESP32 对 JSON 自动使用 L610 Datasize 二进制发布模式。

---

## 四、ESP32 透传路径（零业务改动）

ESP32 `uart_handler_1.c` 已有 `mqtt_publish` / `mqtt_connect` 处理：

```
WS63 → UART1: {"cmd":"mqtt_publish","topic":"v1/gateway/telemetry","payload":"<json>"}
ESP32 → L610:  AT+MQTTPUB=1,"v1/gateway/telemetry",1,0,"<json>"
```

L610 连接 EMQX（Fibocom MQTT AT V1.0.2）：

```
AT+MQTTUSER=1,"<user>","<pass>","esp32_s3cam"
AT+MQTTOPEN=1,"h13f6185.ala.cn-hangzhou.emqxsl.cn",8883,1,60,2
```

> **UseTls 必须为 2**（0=tcp, 1=reserve, 2=tls）。Payload 最长 1024 字节。

ESP32 配置见 `main/modules/4g/l610_config.h`（凭据烧录前填入，与 `dashboard/backend/.env` 一致）。

---

## 五、验证

- [ ] WS63 编译通过
- [ ] BS21E 靠近 WS63 → 日志出现 tag 转发
- [ ] EMQX 控制台 `v1/gateway/telemetry` 可见 `tag_XXX` + `gateway`
- [ ] 30s 后 `gateway` 块字段刷新（同一主题，无独立 heartbeat）
- [ ] Dashboard 后端 subscribe 单主题后 `/api/esp32` 有数据
- [ ] `WS63_CLOUD_MODE` 切回 0 后恢复 WiFi 直连云
