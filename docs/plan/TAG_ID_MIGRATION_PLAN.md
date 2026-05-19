# Tag ID 改造实施报告 v3.0

> **文档版本**: v3.0  
> **最后更新**: 2026-05-19  
> **适用项目**: CAM_AI (ESP32-S3 视觉感知物资管理子节点)  
> **改造目标**: 将资产唯一标识从 MAC 地址升级为 16 位 Tag ID，并实现验证式数量累加机制

---

## ✅ 改造完成情况

所有阶段（除 Phase 0 代码结构重构外）均已完成并合并入主分支。

### Phase 1：数据结构与常量定义 ✅
- `asset_record_t.tag_id[7]` 替代 `mac_address[18]`
- `main.h` / `main.c`: `g_current_mac[18]` → `g_current_tag_id[7]`
- `protocol_handler.c`: `g_ws63_mac[18]` → `g_ws63_tag_id[7]`
- 新增 `CAM_STATE_VERIFYING_EXISTING`、`CAM_STATE_WAITING_VERIFY_CAPTURE`、`CAM_STATE_WAITING_REG_ADD_QTY`
- 新增 `WS63_STATE_VERIFYING`、`WS63_STATE_VERIFY_WAITING`
- `CAM_STATE_WAITING_MAC` → `CAM_STATE_WAITING_TAG_ID`

### Phase 2：Tag ID 验证器 ✅
- `tag_id_validator.c/h`（155行）：validate / normalize / parse / format / get_error
- 替换所有旧MAC验证调用
- 新增 `ERR_INVALID_TAG_ID` 错误码

### Phase 3：验证流程实现 ✅
- `verify_handler.c/h`：start / execute / reset / is_active / is_max_retries
- `verify_config.h`：阈值/权重/重试次数参数化
- 混合相似度算法（0.7×余弦 + 0.3×欧氏）
- UART0/UART1 两路复用

### Phase 4：UART0调试串口改造 ✅
- Tag ID 输入引导（`0x0001-0xFFFF`格式）
- 验证式更新交互引导（4个show函数）
- 验证重试机制（最多3次）
- 数量累加/溢出保护

### Phase 5：文件命名与存储适配 ✅
- 直接使用 `0x0001.dat` 文件名（无需转义）
- 图片命名 `0x0001_front.jpg` / `0x0001_side.jpg` / `0x0001_top.jpg`
- 资产列表显示 `Tag ID: 0x0001` 替代 `MAC: AA:BB:CC:DD:EE:FF`

### Phase 6：配置参数化 ✅
- `verify_config.h` 全部宏定义

### Phase 7：协议文档更新 ✅
- `docs/PROTOCOL.md` / `docs/QUICKSTART.md` / `docs/USER_GUIDE.md`
- `README.md` 版本号 v3.2

---

## 🚧 未实施项

### Phase 0：代码结构重构（文件拆分）
> **说明**：按需求暂不处理，保留现状。

- [ ] 拆分 `protocol_handler.c`（2326行）为子模块
- [ ] 创建 `protocol/`、`asset/`、`verify/`、`tag_id/` 子目录
- [ ] 更新 `CMakeLists.txt`

---

## 📦 核心设计（保留为参考）

### Tag ID 规范
| 项目 | 说明 |
|------|------|
| 格式 | `0x` + 4位十六进制（如`0x0001`） |
| 长度 | 6字符（不含终止符），7字符（含终止符） |
| 范围 | `0x0001` - `0xFFFF`（65,535个唯一标识） |
| 大小写 | 不敏感，存储时统一转为大写 |

### 验证式更新流程
```
register命令
    │
    ├─ Tag ID不存在 → 完整注册（拍摄三视图）
    │
    └─ Tag ID已存在 → 拍摄正视图 → 混合相似度比对
                         │
                    ┌────┴────┐
                  ≥0.75      <0.75
                    │          │
                    ▼          ▼
                 累加数量    拒绝更新
```

### 相似度算法
```c
mixed_sim = 0.7 × cosine_sim + 0.3 × euclidean_sim
```

### 验证配置参数
| 参数 | 默认值 |
|------|--------|
| `VERIFY_THRESHOLD_DEFAULT` | 0.75 |
| `VERIFY_THRESHOLD_HIGH` | 0.85 |
| `VERIFY_THRESHOLD_LOW` | 0.65 |
| `VERIFY_COSINE_WEIGHT` | 0.70 |
| `VERIFY_EUCLIDEAN_WEIGHT` | 0.30 |
| `VERIFY_RETRIES_MAX` | 3 |
| `VERIFY_MODE_DEFAULT` | FAST（仅正视图） |

---

## 🔮 未来扩展（未实施）

- 多视图验证模式（标准/严格）
- 自适应阈值（基于物品类别/历史数据）
- 分层标识体系
- 批量操作支持
- RFID/NFC集成

---

## 📊 性能数据

| 指标 | 数值 |
|------|------|
| Tag ID验证 | <1ms |
| 资产加载 | <50ms |
| 正视图拍摄+特征提取 | ~2.5秒 |
| 相似度计算 | <10ms |
| 验证式更新总耗时 | ~2.6秒（vs 完整注册7.5秒） |
| 单记录占用 | ~20KB（含文件系统 overhead） |

---

## 附录

### 修改的文件清单

| 文件 | 变更 |
|------|------|
| `main/main.c` | 全局变量 `g_current_mac` → `g_current_tag_id`，状态枚举更新，所有 `mac` 字段引用改为 `tag_id` |
| `main/main.h` | `CAM_STATE_WAITING_MAC` → `CAM_STATE_WAITING_TAG_ID`，新增验证状态枚举 |
| `main/modules/system/cmd_handler.c` | 删除旧MAC验证函数，替换为tag_id验证，全状态机更新 |
| `main/modules/system/cmd_handler.h` | 删除旧MAC函数声明，参数 `mac` → `tag_id` |
| `main/modules/system/protocol_handler.h` | 函数参数 `mac` → `tag_id`，新增验证错误码 |
| `main/modules/system/asset_manager.h` | 函数参数 `mac_address` → `tag_id` |
| `main/modules/system/asset_manager.c` | 文件名转义逻辑移除，资产列表显示 `Tag ID:` |
| `main/modules/system/storage_module.h` | 函数参数 `mac_address` → `tag_id` |
| `main/modules/system/tag_id_validator.c/h` | 新增（155行，5个API） |
| `main/modules/system/verify_handler.c/h` | 新增（247行，验证流程实现） |
| `main/modules/system/verify_config.h` | 新增（94行，配置参数宏定义） |
| `docs/PROTOCOL.md` | 所有 `mac` 字段 → `tag_id`，新增验证式更新说明 |
| `docs/QUICKSTART.md` | MAC地址示例替换为Tag ID |
| `docs/USER_GUIDE.md` | 全文档MAC→Tag ID更新 |
| `README.md` | 版本号 v3.1 → v3.2 |

---

**文档版本**: v3.0（改为实施报告）  
**最后更新**: 2026-05-19  
**维护者**: TcXc  
**反馈邮箱**: 202500201056@stumail.sztu.edu.cn
