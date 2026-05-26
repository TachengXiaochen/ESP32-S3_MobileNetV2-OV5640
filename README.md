# ESP32-S3 CAM AI 资产管理系统 v3.3

> 基于 **ESP32-S3 + OV5640摄像头 + MobileNetV2** 的智能资产管理系统，通过三视图加权综合判断实现高精度物品识别和盘点。**v3.3** 核心升级：补全 WS63 协议缺失命令（delete/cancel/get_asset/ping），完善下行命令体系。

---

## ✨ 核心特性

- 🆔 **Tag ID 标识** ⭐v3.2：16位十六进制唯一标识（`0x0001`-`0xFFFF`，支持65,535个资产）
- ✅ **验证式更新** ⭐v3.2：Tag ID已存在时，拍摄正视图进行身份验证后方可累加数量
- 🔍 **混合相似度验证**：余弦+欧氏距离加权评估，阈值默认0.75
- 🎯 **智能识别**：MobileNetV2特征提取 + 多帧融合 + 模糊度检测
- 📊 **三视图分析**：正面/侧面/顶部加权综合置信度评估
- 🔀 **双线程架构**：拍摄与推理分离，响应速度提升37倍
- 💾 **TF卡存储**：完整资产管理（名称、区域、数量）
- 🚪 **出库模式**：仅拍摄正视图快速比对，自动更新库存
- 🗑️ **资产删除**：一键删除资产及关联图片
- 💡 **LED状态指示**：WS2812 RGB实时反馈系统状态
- 📡 **WS63协议**：UART1 JSON通信，支持远程调度（v3.0）
- 📶 **L610 4G模块**：MQTT云端通信 + 主动上报机制（v3.1）

---

## 🚀 快速开始

### 硬件要求

| 组件 | 型号 | 说明 |
|------|------|------|
| 主控芯片 | ESP32-S3 | 双核240MHz，带PSRAM |
| 摄像头 | OV5640 | 500万像素，自动对焦 |
| 存储卡 | MicroSD/TF | FAT32格式，建议≥8GB |
| LED指示灯 | WS2812 | RGB可编程LED（可选） |
| 4G模块 | L610 | 支持MQTT通信（可选，v3.1） |

### 编译烧录

```bash
# 1. 配置项目
idf.py menuconfig

# 2. 编译
idf.py build

# 3. 烧录
idf.py flash monitor
```

### 首次运行

1. **连接串口**：USB转TTL连接UART0（GPIO43/44），波特率115200
2. **输入Tag ID**：系统启动后输入资产Tag ID（格式：`0x0001`-`0xFFFF`）
3. **验证通过**：Tag ID校验成功后自动进入主菜单
4. **开始使用**：选择注册/盘点/出库等功能

---

## 📖 功能说明

### 1. 资产注册（Register）

**流程**：引导拍摄三视图 → 提取特征向量 → 保存到TF卡

**模式A：完整注册（新资产）**
```json
{
  "cmd": "register",
  "tag_id": "0x0001",
  "item_name": "扳手",
  "storage_area": "A",
  "quantity": 50
}
```

**模式B：验证式更新（已有资产累加）** ⭐v3.2
```json
{
  "cmd": "register",
  "tag_id": "0x0001",
  "quantity": 20
}
```
- Tag ID已存在时，必须拍摄正视图进行身份验证
- 相似度≥0.75才允许累加数量
- 验证失败明确拒绝，防止错误操作

### 2. 资产盘点（Inventory）

**流程**：拍摄三视图 → 多帧融合 → 特征匹配 → 返回置信度

**特点**：
- 每帧采集3张图像进行融合
- 拉普拉斯方差算法过滤模糊图像
- 混合相似度算法（余弦+欧氏距离）

### 3. 出库核验（Outbound）

**流程**：仅拍摄正视图 → 快速比对 → 自动扣减库存

**优势**：速度快，适合频繁出入库场景

### 4. 资产删除（Delete）

**功能**：删除资产记录及其所有关联图片

**安全机制**：二次确认，防止误删

---

## 📡 WS63协议支持（v3.0）

### 硬件连接

```
WS63                          ESP32-S3
├── RX ◄──────── GPIO17 (TX) ──┤
├── TX ────────► GPIO18 (RX) ──┤
└── GND ───────── GND ─────────┘
```

**通信参数**：UART1, 115200 bps, 8N1, JSON Lines格式

### 常用命令

| 命令 | 说明 | 示例 |
|------|------|------|
| `register` | 资产注册（含验证式更新） | `{"cmd":"register","tag_id":"0x0001","item_name":"扳手","quantity":50}` |
| `inventory` | 资产盘点 | `{"cmd":"inventory","item_name":"扳手"}` |
| `outbound` | 出库核验 | `{"cmd":"outbound","tag_id":"0x0001","remove_qty":5}` |
| `capture` | 分步拍摄 | `{"cmd":"capture","view":"front"}` |
| `delete` | 删除资产 | `{"cmd":"delete","tag_id":"0x0001"}` |
| `list_assets` | 查询列表 | `{"cmd":"list_assets"}` |
| `sys_info` | 系统信息 | `{"cmd":"sys_info"}` |

### 上行消息

| 类型 | 说明 |
|------|------|
| `capture_progress` | 拍摄进度（含模糊度评分） |
| `task_done` | 任务完成结果 |
| `verification_start` | ⭐验证开始提示（v3.2） |
| `asset_list` | 资产列表 |
| `error` | 错误报告 |

**详细协议规范**：查看 [docs/PROTOCOL.md](docs/PROTOCOL.md)

---

## 📶 L610 4G模块集成（v3.1）

### 硬件连接

```
ESP32-S3                L610 Module
├── GPIO4 (TX) ──────► RX
├── GPIO5 (RX) ◄────── TX
└── GND ────────────── GND
```

**注意**：L610峰值电流可达2A，建议使用独立电源

### MQTT功能

**连接服务器**：
```json
{
  "cmd": "mqtt_connect",
  "host": "mqtt.thingskit.com",
  "port": 1883
}
```

**发布消息**：
```json
{
  "cmd": "mqtt_publish",
  "topic": "device/status",
  "payload": "{\"status\":\"online\"}",
  "qos": 1
}
```

**断开连接**：
```json
{"cmd": "mqtt_disconnect"}
```

### ⭐ 主动上报机制

L610模块可主动向WS63上报事件：
- MQTT意外断开 → `l610_error`
- 模块失联 → `L610_NOT_RESPONDING`
- 网络异常 → `NETWORK_DETACHED`

**优势**：无需轮询，实时性高

### AT指令透传（调试用）

```json
{"cmd": "l610_at", "at": "AT+CSQ"}
```

**常用指令**：
- `AT` - 测试通信
- `AT+CSQ` - 信号质量
- `AT+CGATT?` - 网络附着状态

**详细协议和调试指南**：
- 协议规范：[docs/PROTOCOL.md](docs/PROTOCOL.md) §第11-19章
- 调试指南：[docs/L610_DEBUG_GUIDE.md](docs/L610_DEBUG_GUIDE.md)

---

## 📚 文档导航

**核心文档**：
- 📘 **入门**: [QUICKSTART.md](docs/QUICKSTART.md) | [USER_GUIDE.md](docs/USER_GUIDE.md)
- 📗 **协议**: [PROTOCOL.md](docs/PROTOCOL.md) ⭐统一协议文档（v3.2）
- 📙 **调试**: [L610_DEBUG_GUIDE.md](docs/L610_DEBUG_GUIDE.md) | [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)

**历史文档**: 查看 `docs/archive/` 目录

---

## 🔧 技术架构

### 模块化设计

```
main/
├── modules/
│   ├── camera/          # 摄像头驱动（OV5640）
│   ├── ai/              # AI推理引擎（MobileNetV2）
│   ├── system/          # 系统管理（协议处理、状态机、验证器）
│   │   ├── tag_id/      # ⭐ Tag ID 验证器（v3.2）
│   │   └── verify/      # ⭐ 身份验证模块（v3.2）
│   └── 4g/              # L610 4G模块驱动（v3.1）
└── app_main.c           # 应用入口
```

### 多任务并发

| 任务 | 优先级 | 职责 |
|------|--------|------|
| Camera Task | 5 | 图像采集、多帧融合 |
| AI Inference Task | 4 | 特征提取、相似度计算 |
| Protocol Handler | 5 | UART1命令解析、JSON通信 |
| L610 Heartbeat | 4 | 4G模块心跳检测（v3.1） |
| LED Indicator | 3 | WS2812状态指示 |

### 关键技术

- **标识方式**：16位十六进制Tag ID（`0x0001`-`0xFFFF`）
- **验证算法**：混合相似度 = 0.7×余弦 + 0.3×欧氏，阈值0.75
- **特征提取**：MobileNetV2，1280维特征向量
- **模糊度检测**：拉普拉斯方差算法，阈值80分
- **多帧融合**：每次拍摄采集3帧，取最优特征
- **看门狗管理**：长耗时操作分段执行，定期复位

---

## 📊 性能指标

| 指标 | 数值 | 说明 |
|------|------|------|
| 单次拍摄时间 | ~800ms | 包含3帧采集 |
| 特征提取时间 | ~1.2s | MobileNetV2推理 |
| 相似度计算 | <10ms | 1280维向量对比 |
| 验证式更新 | ~2.5s | 仅需正视图（vs 完整注册7.5s） |
| 识别准确率 | ≥92% | 良好光照条件下 |
| Heap空闲内存 | >50KB | 避免碎片化 |
| TF卡容量 | 7.5GB | 典型8GB卡可用空间 |

---

## ⚠️ 注意事项

1. **供电稳定**：L610模块峰值电流2A，建议使用独立电源
2. **天线连接**：必须连接4G天线，否则信号极弱
3. **SIM卡激活**：确保SIM卡已激活且有流量套餐
4. **TF卡格式**：使用前需格式化为FAT32
5. **环境光线**：建议在200-500lux光照条件下使用
6. **模糊度阈值**：默认80分，可根据实际需求调整
7. **Tag ID格式**：使用 `0x0001`-`0xFFFF` 格式，共65,535个唯一标识

---

## 🔄 版本历史

- **v3.3** (2026-05-26): ⭐ 协议命令补全 + Tag ID 适配（delete/get_asset 改用 tag_id 字段，移除状态限制）
- **v3.2** (2026-05-19): ⭐ Tag ID 改造（MAC地址→16位Tag ID，验证式数量累加，混合相似度验证）
- **v3.1** (2026-05-10): L610 4G模块完整集成（MQTT云端通信、主动上报机制、AT重试、Payload保护、资源清理）
- **v3.0** (2026-04-29): WS63协议支持（JSON格式UART通信、分步拍摄控制）
- **v2.6** (2026-04-28): 模糊度检测、混合相似度算法
- **v2.5** (2026-04-25): 双线程架构、出库模式、资产详细信息
- **v2.x**: 基础资产管理系统功能

---

## 📞 技术支持

- **问题反馈**: [GitHub Issues](https://github.com/your-repo/CAM_AI/issues)
- **技术文档**: [docs/](docs/) 目录
- **邮箱**: 202500201056@stumail.sztu.edu.cn

---

**许可证**: MIT  
**维护者**: TcXc  
**最后更新**: 2026-05-26