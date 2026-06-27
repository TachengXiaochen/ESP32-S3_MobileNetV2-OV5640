# 星闪资产盘点系统 — Web 可视化仪表盘规划

> **文档版本**: v3.4  
> **日期**: 2026-06-26  
> **赛道**: 2026 全国大学生物联网设计竞赛 — **广和通 IoT 方向**（LTE CAT1 / ADP-L610-Arduino）  
> **状态**: 前端 v3 完成；云端 **EMQX Cloud**；**方案 A** 单主题 `v1/gateway/telemetry`；后端 MQTT 直订阅待改造  

### 部署状态

| 组件 | 地址 | 状态 |
|------|------|:--:|
| MQTT Broker | **EMQX Cloud**（见 §1.1） | ⏳ 待联调 |
| MQTT Broker（旧） | `tcp://47.107.120.9:1883` Mosquitto | ⚠️ 已弃用，仅作历史参考 |
| 仪表盘前端 v3 | `localhost:3001` / React + Vite | ✅ 重构完成 |
| 仪表盘后端 | `localhost:3000` / ThingsKit HTTP 轮询 | ⚠️ 待改 EMQX MQTT 直订阅 |
| 一键启停 | `dashboard/start.ps1` · `stop.ps1` | ✅ |
| ESP32 端 | `l610_config.h` 已指向 EMQX MQTTS；凭据烧录前填入 | ⏳ 待联调 |
| WS63 端 | mqtt_publish AT 透传 | ⏳ 待对接 EMQX |

---

## 一、云端 MQTT 方案（EMQX Cloud）

### 1.1 Broker 配置（当前计划）

> ⚠️ **敏感信息**：凭据存放在 `dashboard/backend/.env`（已加入 `.gitignore`），本文档仅保留占位符。新成员请复制 `.env.example` → `.env` 后填入真实值。

| 项 | 值 |
|----|-----|
| Broker 地址 | `h13f6185.ala.cn-hangzhou.emqxsl.cn` |
| 端口 | `8883`（SSL / MQTTS） |
| 协议 | `mqtts://` |
| 用户名 | `<MQTT_USERNAME>`（见 `.env`） |
| 密码 | `<MQTT_PASSWORD>`（见 `.env`） |
| 控制台 | [EMQX Cloud 控制台](https://cloud.emqx.com/)（队内账号管理） |

**连接 URL 示例**（后端 Node.js / 调试工具）：

```
mqtts://<MQTT_USERNAME>:<MQTT_PASSWORD>@h13f6185.ala.cn-hangzhou.emqxsl.cn:8883
```

**ESP32 / L610 AT 侧需同步修改**（`main/modules/4g/l610_config.h`）：

| 宏 | 旧值（Mosquitto） | 新值（EMQX Cloud） |
|----|-------------------|-------------------|
| `L610_MQTT_BROKER_HOST` | `47.107.120.9` | `h13f6185.ala.cn-hangzhou.emqxsl.cn` |
| `L610_MQTT_BROKER_PORT` | `1883` | `8883` |
| `L610_MQTT_USE_TLS` | `0` | **`2`**（Fibocom 手册：0=tcp, 1=reserve, **2=tls**） |
| `L610_MQTT_USERNAME` | `""` | 见 `.env` → `MQTT_USERNAME` |
| `L610_MQTT_PASSWORD` | `""` | 见 `.env` → `MQTT_PASSWORD` |

### 1.2 赛题合规说明

广和通 IoT 方向配套课程区分：

| 类型 | 含义 | 本项目 |
|------|------|--------|
| **私有云工程** | 自建 Broker（Mosquitto / 自部署 EMQX） | 旧方案，已切换 |
| **公有云工程** | 华为 / 阿里 / 腾讯 / 百度 IoT 平台 | 未采用 |
| **托管 MQTT 云** | EMQX Cloud 等 SaaS Broker | **✅ 当前方案** |

EMQX Cloud **不是** PDF 里的「私有云」，但符合 IoT 开放命题要求：

- ✅ 广和通 **L610 CAT1** 作传输层（ADP-L610-Arduino + ESP32-S3 UART AT）
- ✅ **MQTT over TLS** 终端上云（安全可靠传输）
- ✅ 构建 **物联云 / 物联物 / 物联人**（标签—云—Web 仪表盘—操作员）

答辩话术建议：「终端经 L610 4G，通过 MQTTS 接入 EMQX Cloud，后端订阅后供 Web 仪表盘展示。」

### 1.3 方案对比（为何从 Mosquitto / ThingsKit 切换）

| | ThingsKit（当前后端） | 自建 Mosquitto（旧计划） | **EMQX Cloud（当前计划）** |
|---|---|---|---|
| 数据流 | HTTP 轮询 5s | MQTT 直订阅 | **MQTT 直订阅** |
| 实时性 | ~5s | 毫秒级 | **毫秒级** |
| TLS | 依赖平台 | 需自配证书 | **✅ 8883 内置** |
| 运维 | 第三方平台 | 自管 ECS | **托管，免运维** |
| 赛题 | 可用但非 L610 课程示例 | 私有云工程示例 | **MQTT 上云，合规** |
| 状态 | ⚠️ 后端仍在用 | ⚠️ 已弃用 | **⏳ 目标方案** |

**结论**：弃用 ThingsKit 轮询；云端 Broker 统一为 **EMQX Cloud（MQTTS 8883）**；上云主题为 **`v1/gateway/telemetry`（方案 A，单主题）**。

### 1.4 L610 MQTT AT 指令（Fibocom V1.0.2）

> 参考：`1.1--L610产品文档/4-软件/AT命令/FIBOCOM L610 Series AT Commands_MQTT_V1.0.2.pdf`

本项目 ESP32 仅做 **AT 透传**，WS63 通过 UART1 `mqtt_connect` / `mqtt_publish` 驱动 L610。终端侧无需 `AT+MQTTSUB`（无云端下行）。

| 步骤 | AT 指令 | 说明 |
|------|---------|------|
| 1 | `AT+MQTTUSER=1,"<user>","<pass>","esp32_s3cam"` | 连接前设置；Client id 槽位为 **1 或 2** |
| 2 | `AT+MQTTOPEN=1,"<host>",8883,1,60,2` | 最后一参 **UseTls=2** 启用 TLS（非 1） |
| 3 | `AT+MQTTPUB=1,"v1/gateway/telemetry",1,0,"<json>"` | Payload 最长 **1024** 字节 |
| 4 | `AT+MQTTCLOSE=1` | 断开连接 |

异步 URC：`+MQTTOPEN: 1,1`（成功）、`+MQTTPUB: 1,1`（发布成功）、`+MQTTBREAK: 1,<cause>`（意外断开）。

代码映射：`main/modules/4g/l610_mqtt.c` → `l610_mqtt_set_user()` / `l610_mqtt_connect()` / `l610_mqtt_publish()`。

---

## 1.5 上云 Payload — 方案 A（已选定）

**决策**：ESP32 状态**不**单独 heartbeat 到 `nelink/esp32/status`，而是并入 **`v1/gateway/telemetry`** 的 ThingsKit 网关 JSON，与标签子设备同包发布。

### 主题

| 主题 | 方向 | 说明 |
|------|------|------|
| **`v1/gateway/telemetry`** | WS63 → L610 透传 → EMQX | **唯一上云主题**（标签 + gateway 子设备） |
| `v1/gateway/rpc/+` | 云 → 设备 | **不需要**（仪表盘只读，无云端 RPC 下发） |

### Payload 格式

```json
{
  "tag_001": [{"tag_id":1,"zone":"A1","item":"Type-C","qty":50,"status":2,"battery":95}],
  "gateway": [{"camera_ready":true,"storage_ready":true,"free_heap":185632,"storage_total_mb":8,"storage_free_mb":6,"state":"idle"}]
}
```

| Key | 来源 | 说明 |
|-----|------|------|
| `tag_XXX` | BS21E → WS63 SLE | 子设备遥测，XXX 为三位 tag_id |
| `gateway` | WS63 每 30s `sys_info` 查 ESP32 | ESP32 作为网关子设备状态（非独立 topic） |

### WS63 发布策略

1. **标签变更**：立即 `mqtt_publish` → `v1/gateway/telemetry`，payload 含变更的 `tag_XXX` + 缓存的 `gateway` 块。
2. **定时 30s**（IDLE 时）：`sys_info` → 更新 `gateway` 缓存 → 合并所有已知 `tag_XXX` + `gateway` 后单次发布。
3. **ESP32 角色**：仅响应 `sys_info` 与 AT 透传，**不**自行 `l610_mqtt_publish` 心跳。

### 后端解析

订阅 **`v1/gateway/telemetry` 唯一主题**：

- key 匹配 `tag_\d+` → 更新 `tag_cache` + SQLite
- key 为 `gateway` → 更新 `esp32State`（`/api/esp32`）

---

## 〇、系统架构

### 角色分工

```
┌──────────────────────────────────────────────────────────────────┐
│                   手持终端（一体化）                                │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │   WS63（主控）                     4.3" 串口屏（HMI）         │ │
│  │   — SLE 收 BS21E 标签广播                                    │ │
│  │   — 业务编排（入库/出库/盘点/寻物）                            │ │
│  │   — 汇总所有数据                                              │ │
│  │   — 通过 UART1 JSON → ESP32（含 mqtt_publish 指令）           │ │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │   ESP32-S3（L610 AT 透传 + 摄像头 AI）                       │ │
│  │   — OV5640 三视图 + MobileNetV2 推理                         │ │
│  │   — TF 卡资产管理                                             │ │
│  │   — UART2 AT 透传 → L610                                     │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
         │ UART AT 透传           │ SLE 广播（12B）
         ▼                         ▼
    ┌─────────┐            ┌──────────────┐
    │ L610 4G │            │ BS21E 标签 ×N │
    │ ADP-L610│            └──────────────┘
    └────┬────┘
         │ MQTTS :8883
         ▼
    ┌─────────────────────────┐
    │ EMQX Cloud Broker       │  ← 杭州节点（托管）
    │ h13f6185.ala.cn-hangzhou│
    │ .emqxsl.cn              │
    └────────┬────────────────┘
             │ mqtt.subscribe
    ┌────────┴────────────────┐
    │ Dashboard 后端 :3000    │  Node.js（开发机 / 服务器）
    │ Dashboard 前端 :3001    │  React + Vite
    └─────────────────────────┘
```

### 一句话总结

> **WS63 是主控大脑，ESP32 是视觉节点 + L610 AT 透传通道。** 业务在串口屏完成；Web 仪表盘是纯展示层。

### 数据流

```
BS21E → SLE → WS63 汇总 → UART1 mqtt_publish → ESP32 透传 → L610 AT
     → EMQX Cloud (MQTTS) → Dashboard 后端 subscribe → REST API → React 前端
```

---

## 二、Web 架构详解

### 2.1 仓库结构

```
dashboard/
├── frontend-react/     # ✅ 当前前端（React 19 + Vite + ECharts）
├── frontend/           # ⚠️ 旧版 Vue3 + Naive UI（已弃用）
├── backend/            # Node.js Express API
├── start.ps1 / stop.ps1
└── scripts/            # 启停子脚本
```

### 2.2 三层关系

```
┌─────────────────────────────────────────────────────────────────────┐
│              前端 (React + Vite)  localhost:3001                     │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│   │  仪表盘   │  │  标签    │  │  历史    │  │  系统    │          │
│   └─────┬────┘  └─────┬────┘  └─────┬────┘  └─────┬────┘          │
│         └─────────────┴──────┬──────┴─────────────┘                │
│                   HTTP 轮询 (每 5s)                                  │
│                   GET /api/tags /api/stats /api/esp32 /api/events    │
│                   GET /api/history?tag_id=X&range=1h  (待后端实现)   │
└──────────────────────────────┼──────────────────────────────────────┘
                               │ Vite proxy /api → :3000
┌──────────────────────────────┼──────────────────────────────────────┐
│              后端 (Node.js + Express)  localhost:3000                │
│   ① MQTT 订阅 (目标)          ② tag_cache 内存        ③ SQLite (计划) │
│   v1/gateway/telemetry       Map<id, entry>          历史遥测        │
│   (tag_* + gateway 同包)     esp32State ← gateway key                │
│   ④ REST API → 前端                                                  │
└──────────────────────────────┼──────────────────────────────────────┘
                               │ mqtts:// :8883
┌──────────────────────────────┼──────────────────────────────────────┐
│                   EMQX Cloud MQTT Broker                             │
└──────────────────────────────┼──────────────────────────────────────┘
                               ▲ MQTTS publish
                         L610 4G ← ESP32 AT 透传 ← WS63
```

**前端从不直连 MQTT**。浏览器走 HTTP 轮询后端 REST API。

### 2.3 后端取数据（目标：EMQX 直订阅）

```
EMQX Cloud               后端                       存储
   │                         │                         │
   │── v1/gateway/telemetry ▶│ (mqtt.on('message'))    │
   │   JSON: tag_* + gateway │── tag_cache.update() ──▶│ 内存 Map
   │                         │── esp32State (gateway) ▶│ 内存
   │                         │── db.insert() ────────▶│ SQLite (计划)
```

**当前实现（过渡）**：`server.js` 仍通过 `thingskit_api.js` 每 5s HTTP 轮询，需改造为 EMQX MQTT 订阅。

### 2.4 双写策略（计划）

| | 内存 tag_cache | SQLite |
|---|---|---|
| 用途 | 实时 KPI、标签表格 | 历史趋势 `/api/history` |
| 存什么 | 每标签最新一条 | 每次遥测追加 |
| 重启 | 丢失（可从 SQLite 恢复最新） | 持久保留 |

### 2.5 MQTT 主题（方案 A）

| 主题 | 发布方 | 后端动作 |
|------|--------|----------|
| **`v1/gateway/telemetry`** | WS63 合并 tag + gateway 后 L610 透传 | 解析 `tag_*` → cache/SQLite；解析 `gateway` → esp32State |

> 不再使用 `nelink/tag/telemetry`、`nelink/esp32/status`。不下发 `v1/gateway/rpc/+`，后端无需 subscribe RPC。

---

## 三、改动清单

### 3.1 ESP32 端

#### 待改（EMQX Cloud）

| # | 文件 | 改动 | 状态 |
|---|------|------|:--:|
| 1 | `l610_config.h` | EMQX、Kconfig、UseTls=2 | ✅ |
| 2 | `l610_mqtt.c` | CGATT、Datasize 发布、重连 host | ✅ |
| 3 | 凭据 | `sdkconfig.defaults.local` / menuconfig | ✅ |

#### 架构约束（保持）

| # | 文件 | 说明 | 状态 |
|---|------|------|:--:|
| 1 | `uart_handler_1.c` | `mqtt_publish` / `mqtt_connect` / `l610_status` AT 透传 | ✅ |
| 2 | `l610_manager.c` | MQTT 由 WS63 `mqtt_connect` 发起；重连沿用上次 host | ✅ |
| 3 | ESP32 角色 | 只做 AI + AT 透传，不自行上云心跳 | ✅ |

### 3.2 WS63 端

| # | 改动 | 状态 |
|---|------|:--:|
| 1 | 标签变更 → `mqtt_publish` topic=`v1/gateway/telemetry`，payload 含 `tag_XXX` + 缓存 `gateway` | ⏳ |
| 2 | `biz_sle.c` — IoT 赛道 UART1 AT 透传 | ✅ |
| 3 | 定时 30s `sys_info` → 更新 `gateway` 块 → 合并全量 tag + gateway 单次发布（**非**独立 status topic） | ⏳ |
| 4 | `mqtt_connect` 参数指向 EMQX Cloud（经 ESP32 透传） | ⏳ |

### 3.3 仪表盘前端 v3（✅ 已完成）

**框架**: React 19 + Vite 8 + react-router-dom + ECharts  
**位置**: `dashboard/frontend-react/`

| 路由 | 页面 | 内容 |
|------|------|------|
| `/` | Home | 落地页：五端拓扑、流程、指标 |
| `/app` | Dashboard | KPI + ECharts 饼图 + 事件日志 |
| `/app/tags` | Tags | 标签表格 + 搜索 |
| `/app/history` | History | 标签 + 时间范围 + 折线图 |
| `/app/system` | System | ESP32 + 服务器状态 |

**设计系统**: Linear 风格 · 主色 `#00B48C` · Space Grotesk + JetBrains Mono

**本地开发**:

```powershell
.\dashboard\start.ps1          # 后端 :3000 + 前端 :3001
.\dashboard\start.ps1 -OpenBrowser
.\dashboard\stop.ps1
```

### 3.4 仪表盘后端（⏳ 待改造）

| 项目 | 当前 | 目标 |
|------|------|------|
| 数据源 | ThingsKit HTTP 轮询 | **EMQX Cloud MQTT 订阅** |
| Broker | — | `h13f6185.ala.cn-hangzhou.emqxsl.cn:8883` MQTTS |
| 凭据 | — | `dashboard/backend/.env`（不入库） |
| 存储 | 纯内存 `tag_cache` | 内存 + **SQLite** 历史 |
| API | tags/stats/esp32/events ✅ | `/api/history` ❌ 未实现 |
| ESP32 状态 | cache 默认值，无写入 | MQTT `gateway` key 更新 esp32State |

**后端环境变量**（复制 `dashboard/backend/.env.example` → `.env`）：

```env
MQTT_BROKER_URL=mqtts://h13f6185.ala.cn-hangzhou.emqxsl.cn:8883
MQTT_USERNAME=<MQTT_USERNAME>
MQTT_PASSWORD=<MQTT_PASSWORD>
MQTT_TOPIC=v1/gateway/telemetry
HTTP_PORT=3000
```

---

## 四、REST API 契约

| 方法 | 路径 | 说明 | 状态 |
|------|------|------|:--:|
| GET | `/api/tags` | 全部标签 | ✅ |
| GET | `/api/tags/:id` | 单标签 | ✅ |
| GET | `/api/stats` | KPI 汇总 | ✅ |
| GET | `/api/esp32` | ESP32 状态 | ⚠️ 数据未接通 |
| GET | `/api/events?limit=N` | 事件日志 | ✅ |
| GET | `/api/health` | 后端健康 | ✅ |
| GET | `/api/history?tag_id=X&range=1h` | 历史趋势 | ❌ 待实现 |

---

## 五、部署方案

### 5.1 开发环境（本地）

```powershell
# 项目根目录
.\dashboard\start.ps1 -OpenBrowser
# 前端 http://localhost:3001  后端 http://localhost:3000
# Vite 将 /api 代理到后端
```

### 5.2 生产环境（规划）

| 层 | 方案 |
|----|------|
| MQTT | **EMQX Cloud**（已托管，无需自装 Broker） |
| 后端 | Node.js + PM2，`subscribe` EMQX → 暴露 REST :3000 |
| 前端 | `npm run build` → Nginx 静态托管 |
| 旧 ECS Mosquitto | 可停用或仅作备用 |

---

## 六、不在范围内的功能

| 功能 | 原因 |
|------|------|
| 远程盘点 / 出库 / 入库 | 串口屏完成，仪表盘只读 |
| ESP32 摄像头远程控制 | 同上 |
| ThingsKit 集成 | 改为 EMQX Cloud MQTT |
| ESP32 SoftAP Web (`main/modules/web`) | 板端调试专用，与云端仪表盘分离 |

---

## 七、实施优先级

| 阶段 | 内容 | 状态 |
|------|------|:--:|
| **P0** | 前端 React v3 重构 | ✅ |
| **P0** | 文档确定 EMQX Cloud 配置（§1.1） | ✅ |
| **P0** | ESP32 `l610_config.h` 改 MQTTS + EMQX（凭据烧录前填入） | ⏳ 配置✅ / 联调⏳ |
| **P0** | L610 联调 EMQX Cloud 8883 连接 | ⏳ |
| **P1** | 后端改 EMQX MQTT 订阅，移除 ThingsKit | ⏳ |
| **P1** | SQLite + `/api/history` | ⏳ |
| **P1** | WS63 30s sys_info 合并进 `v1/gateway/telemetry`（方案 A） | ⏳ |
| **P2** | 生产 Nginx + PM2 部署 | ⏳ |
| **P3** | OTA 固件升级 | 📅 远期 |

---

## 八、验证清单

- [ ] L610 MQTTS 连接 `h13f6185.ala.cn-hangzhou.emqxsl.cn:8883` 成功（`MQTTOPEN` UseTls=**2**）
- [ ] WS63 `mqtt_publish` → `v1/gateway/telemetry` → EMQX 控制台可见（含 `tag_XXX` + `gateway`）
- [ ] WS63 30s `sys_info` 更新后，同一主题 payload 中 `gateway` 块刷新（无独立 heartbeat topic）
- [ ] 后端 subscribe **`v1/gateway/telemetry`**，`GET /api/tags` / `/api/stats` / `/api/esp32` 数据正确
- [ ] 前端 `.\dashboard\start.ps1` 启动，KPI 5s 刷新
- [ ] `/api/history` 返回折线图数据
- [ ] 标签 30s 无数据 → 状态变「离线」
- [ ] EMQX Cloud 控制台确认 Client 在线、消息速率正常

---

## 九、变更记录

| 版本 | 日期 | 变更 |
|------|------|------|
| v3.2 | 2026-06-24 | 前端 React 重构；规划 Mosquitto 自建 Broker |
| v3.3 | 2026-06-26 | 云端切换 **EMQX Cloud MQTTS**；更新架构/后端计划；新增一键启停；弃用 Mosquitto/ThingsKit 为目标态 |
| v3.4 | 2026-06-26 | **方案 A**：ESP32 状态并入 `v1/gateway/telemetry`；L610 AT 手册 TLS=2；ESP32 `l610_config.h` 已更新 |
