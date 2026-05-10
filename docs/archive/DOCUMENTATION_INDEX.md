# CAM_AI 项目文档导航

> **项目版本**: v3.1  
> **最后更新**: 2026-05-10  
> **适用平台**: ESP32-S3 + OV5640摄像头 + L610 4G模块  

---

## 📖 快速开始

### 🚀 新用户？从这里开始

1. **[README.md](../README.md)** - 项目总览、核心特性、快速入门
2. **[docs/QUICKSTART.md](QUICKSTART.md)** - 5分钟快速上手指南
3. **[docs/USER_GUIDE.md](USER_GUIDE.md)** - 完整用户手册

### 🔧 开发者？查看这些

1. **[docs/WS63_ESP32_PROTOCOL.md](WS63_ESP32_PROTOCOL.md)** - 主通信协议规范（UART1）
2. **[docs/WS63_ESP32_L610_PROTOCOL.md](WS63_ESP32_L610_PROTOCOL.md)** - L610扩展协议（UART2 + MQTT）
3. **[docs/L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md)** - L610调试与测试指南
4. **[docs/ALGORITHM_OPTIMIZATION.md](ALGORITHM_OPTIMIZATION.md)** - AI算法优化说明

### ❓ 遇到问题？

1. **[docs/TROUBLESHOOTING.md](TROUBLESHOOTING.md)** - 常见问题与解决方案
2. **[docs/BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md)** - 编译与烧录速查表
3. **[docs/CHANGELOG_v3.1.md](CHANGELOG_v3.1.md)** - v3.1版本更新详情

---

## 📂 文档分类索引

### 📘 入门文档（新手必读）

| 文档 | 说明 | 阅读时间 |
|------|------|----------|
| [README.md](../README.md) | 项目介绍、核心特性、硬件连接 | 10分钟 |
| [QUICKSTART.md](QUICKSTART.md) | 环境搭建、编译烧录、首次运行 | 5分钟 |
| [USER_GUIDE.md](USER_GUIDE.md) | 完整使用手册、操作指南 | 30分钟 |

---

### 📗 协议规范（开发参考）

| 文档 | 说明 | 适用场景 |
|------|------|----------|
| [WS63_ESP32_PROTOCOL.md](WS63_ESP32_PROTOCOL.md) | WS63 ↔ ESP32-S3 通信协议（v3.1） | UART1 JSON通信开发 |
| [WS63_ESP32_L610_PROTOCOL.md](WS63_ESP32_L610_PROTOCOL.md) | L610 4G模块扩展协议（v1.0）⭐NEW | MQTT云端通信开发 |
| [ESP32_TO_WS63_PLAN.md](ESP32_TO_WS63_PLAN.md) | 系统集成设计方案 | 架构理解 |

**协议选择指南**：
- 📡 **仅需本地控制** → 阅读 `WS63_ESP32_PROTOCOL.md`
- 🌐 **需要云端通信** → 额外阅读 `WS63_ESP32_L610_PROTOCOL.md`
- 🔍 **调试L610模块** → 参考 `L610_DEBUG_GUIDE.md`

---

### 📙 技术文档（深入理解）

| 文档 | 说明 | 目标读者 |
|------|------|----------|
| [ALGORITHM_OPTIMIZATION.md](ALGORITHM_OPTIMIZATION.md) | AI识别算法优化策略 | AI工程师 |
| [L610_4G_INTEGRATION_PLAN.md](L610_4G_INTEGRATION_PLAN.md) | L610集成方案设计 | 系统架构师 |
| [L610_AT_REFERENCE.md](L610_AT_REFERENCE.md) | L610 AT指令参考手册 | 驱动开发者 |
| [UPGRADE_ESP_IDF.md](UPGRADE_ESP_IDF.md) | ESP-IDF升级指南 | 维护人员 |

---

### 📕 调试与测试（问题排查）

| 文档 | 说明 | 使用场景 |
|------|------|----------|
| [L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md) ⭐NEW | L610完整调试流程 | 4G模块调试 |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | 通用故障排查指南 | 日常问题处理 |
| [BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md) | 编译与烧录速查 | 快速部署 |
| [CHANGELOG_v3.1.md](CHANGELOG_v3.1.md) ⭐NEW | v3.1版本变更详情 | 版本升级参考 |

---

### 📒 历史文档（参考）

| 文档 | 说明 | 状态 |
|------|------|------|
| [L610_4G_INTEGRATION_PLAN.md](L610_4G_INTEGRATION_PLAN.md) | L610集成计划（已实施） | 📌 归档 |
| [ESP32_TO_WS63_PLAN.md](ESP32_TO_WS63_PLAN.md) | ESP32集成计划（已实施） | 📌 归档 |

---

## 🎯 按任务查找文档

### 任务1：首次部署系统

**推荐文档顺序**：
1. [README.md](../README.md) - 了解项目
2. [QUICKSTART.md](QUICKSTART.md) - 环境搭建
3. [USER_GUIDE.md](USER_GUIDE.md) - 学习操作
4. [BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md) - 编译烧录

**预计时间**：1小时

---

### 任务2：开发WS63通信功能

**推荐文档顺序**：
1. [WS63_ESP32_PROTOCOL.md](WS63_ESP32_PROTOCOL.md) - 协议规范
2. [README.md](../README.md) §WS63协议支持章节 - 使用示例
3. [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - 常见问题

**关键命令**：
```json
{"cmd":"register","mac":"AA:BB:CC:DD:EE:FF",...}
{"cmd":"inventory","item_name":"扳手",...}
{"cmd":"outbound","remove_qty":5,...}
```

---

### 任务3：集成L610 4G模块

**推荐文档顺序**：
1. [WS63_ESP32_L610_PROTOCOL.md](WS63_ESP32_L610_PROTOCOL.md) - 协议规范
2. [L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md) - 调试流程
3. [L610_AT_REFERENCE.md](L610_AT_REFERENCE.md) - AT指令参考
4. [CHANGELOG_v3.1.md](CHANGELOG_v3.1.md) - 实现细节

**关键命令**：
```json
{"cmd":"mqtt_connect","host":"mqtt.thingskit.com","port":1883}
{"cmd":"mqtt_publish","topic":"test","payload":"Hello"}
{"cmd":"l610_at","at":"AT+CSQ"}
```

---

### 任务4：调试AI识别问题

**推荐文档顺序**：
1. [ALGORITHM_OPTIMIZATION.md](ALGORITHM_OPTIMIZATION.md) - 算法原理
2. [USER_GUIDE.md](USER_GUIDE.md) §智能盘点模式 - 操作流程
3. [TROUBLESHOOTING.md](TROUBLESHOOTING.md) §AI相关问题 - 故障排查

**关键指标**：
- 置信度阈值：≥0.75
- 模糊度评分：≥80分
- 特征向量维度：1280维

---

### 任务5：系统故障排查

**常见问题索引**：

| 问题类型 | 参考文档 | 章节 |
|---------|---------|------|
| 编译失败 | [BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md) | 常见问题 |
| 烧录失败 | [BUILD_CHEATSHEET.md](BUILD_CHEATSHEET.md) | 烧录故障 |
| UART通信异常 | [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | UART问题 |
| L610无响应 | [L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md) | §6.1 |
| MQTT连接失败 | [L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md) | §6.2 |
| AI识别率低 | [ALGORITHM_OPTIMIZATION.md](ALGORITHM_OPTIMIZATION.md) | 优化策略 |
| 内存泄漏 | [L610_DEBUG_GUIDE.md](L610_DEBUG_GUIDE.md) | §6.4 |
| 看门狗重启 | [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | 看门狗问题 |

---

## 📊 文档统计

| 类别 | 数量 | 总行数 |
|------|------|--------|
| 入门文档 | 3 | ~1500行 |
| 协议规范 | 2 | ~2000行 |
| 技术文档 | 4 | ~1200行 |
| 调试指南 | 4 | ~2500行 |
| 历史文档 | 2 | ~800行 |
| **总计** | **15** | **~8000行** |

---

## 🔄 文档更新记录

| 日期 | 版本 | 更新内容 |
|------|------|----------|
| 2026-05-10 | v3.1 | 新增L610相关文档（协议、调试指南、更新日志） |
| 2026-04-29 | v3.0 | 新增WS63协议文档 |
| 2026-04-28 | v2.5 | 新增双线程架构说明 |
| 2026-04-25 | v2.3 | 初始文档体系建立 |

---

## 💡 文档使用建议

### ✅ 最佳实践

1. **按需阅读**：根据当前任务选择对应文档，无需通读所有文档
2. **交叉引用**：遇到不理解的概念时，查阅相关文档的链接
3. **实践验证**：边读边做，通过实际操作加深理解
4. **反馈改进**：发现文档错误或遗漏时，及时提交Issue

### ⚠️ 注意事项

1. **版本匹配**：确保阅读的文档版本与代码版本一致
2. **协议优先**：开发前务必仔细阅读协议文档，避免格式错误
3. **调试指南**：遇到问题先查阅调试指南，再搜索其他资源
4. **历史记录**：重要决策和变更原因记录在CHANGELOG中

---

## 📞 获取帮助

- **项目主页**: [GitHub Repository](https://github.com/your-repo/CAM_AI)
- **问题反馈**: [Issues](https://github.com/your-repo/CAM_AI/issues)
- **技术支持**: support@cam-ai.com
- **社区论坛**: [Discussions](https://github.com/your-repo/CAM_AI/discussions)

---

**文档维护者**: CAM_AI开发团队  
**最后更新**: 2026-05-10  
**文档版本**: v1.0
