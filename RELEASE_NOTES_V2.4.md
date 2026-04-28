# ESP32-S3 CAM AI 版本发布说明

## 📦 V2.5 - 出库模式与多线程优化版（2026-04-28）⭐NEW

### 🎉 重大更新

本次V2.5版本在V2.4基础上进行了**重要功能增强**，新增了**出库模式**、**资产详细信息管理**和**拍摄推理分离架构**，显著提升了系统的实用性和性能。

---

## ✨ 新增功能详解

### 1. 🚪 出库模式（Outbound Mode）

**功能描述**：专门用于资产出库管理的独立模式，仅需拍摄正视图进行快速比对，自动更新库存数量。

**核心价值**：
- ✅ **快速出库**：仅拍摄正面视图，耗时从~25秒降至~7.5秒
- ✅ **智能数量管理**：自动计算并更新剩余库存
- ✅ **出库记录保存**：保留原始图片作为出库凭证
- ✅ **零库存自动删除**：数量归零时自动删除资产记录
- ✅ **双重验证**：先比对再更新，防止错误出库

**使用方法**：
```bash
o                          # 进入出库模式
AA:BB:CC:DD:EE:FF         # 输入要出库的MAC地址
# 系统显示资产信息：
========== OUTBOUND MODE ==========
  MAC: AA:BB:CC:DD:EE:FF
  Item: Wooden Chair
  Area: A
  Stock: 10
===================================
[GUIDE] Input quantity to remove: 
5                          # 输入出库数量
# 系统引导拍摄：
========== OUTBOUND ============
  MAC:      AA:BB:CC:DD:EE:FF
  Remove:   5
  [STEP 1/1] Capture FRONT view
           -> Send 'f' to capture
====================================
f                          # 拍摄正视图
# 系统自动比对并更新：
========== OUTBOUND RESULT ==========
  [FRONT VIEW]
    Cosine:      0.9234
    Euclidean:   0.8876
    Mixed:       0.9127
    Confidence:  0.9500
  ----------------------------------------
  Threshold:    0.75
  ✅ MATCH - Same Asset
  MAC: AA:BB:CC:DD:EE:FF
  Original Qty: 10
  Remove Qty:   5
=========================================

✅ OUTBOUND COMPLETE!
  Removed: 5 | Remaining: 5
  MAC: AA:BB:CC:DD:EE:FF
  Original image saved.
  Camera: POWER OFF
```

**技术实现**：
- 状态机：`CAM_STATE_WAITING_OUT_MAC` → `CAM_STATE_WAITING_OUT_QTY` → `CAM_STATE_READY_OUT`
- 命令流程：`CMD_OUTBOUND_ANALYZE` → `CMD_OUTBOUND_UPDATE_QTY`
- 视图数：仅1个（front），通过`g_total_views = 1`控制
- 全局变量：`g_is_outbound_mode`、`g_outbound_quantity`、`g_outbound_original_qty`

**边界处理**：
- 出库数量 ≥ 当前库存：数量归零，自动删除资产
- 比对失败：不更新数量，返回主菜单
- 资产不存在：提示用户先注册

---

### 2. 📋 资产详细信息管理

**功能描述**：入库注册时收集物品名称、存放区域和数量信息，列表展示时完整显示这些详细信息。

**核心价值**：
- ✅ **完整资产档案**：不再只有MAC地址，包含丰富的业务信息
- ✅ **快速识别**：通过物品名称而非MAC地址识别资产
- ✅ **区域管理**：支持A-Z存放区域分类
- ✅ **库存跟踪**：实时掌握每个资产的库存数量
- ✅ **出库基础**：为出库模式提供数量管理基础

**注册流程升级**：
```bash
r                          # 进入注册模式
# Step 1: 输入MAC地址
AA:BB:CC:DD:EE:FF
# Step 2: 输入物品名称
Wooden Chair
# Step 3: 输入存放区域（单个字母A-Z）
A
# Step 4: 输入数量（正整数）
10
# 系统显示摘要：
========== REGISTRATION SUMMARY ==========
  MAC:          AA:BB:CC:DD:EE:FF
  Item Name:    Wooden Chair
  Storage Area: A
  Quantity:     10
===========================================
[SYSTEM] Initializing camera...
```

**列表显示升级**：
```bash
l                          # 列出所有资产
[ASSET LIST]

=== Storage Information ===
  Total: 7580.00 MB
  Used:  0.15 MB (0.0%)
  Free:  7579.85 MB (100.0%)
===========================

=== Registered Assets (SD Card) ===
  [1] MAC: AA:BB:CC:DD:EE:FF | Wooden Chair         | A | 10
  [2] MAC: 11:22:33:44:55:66 | Steel Bolt M8        | B | 100
  [3] MAC: FF:EE:DD:CC:BB:AA | Plastic Container    | C | 5
Total: 3 assets
========================
```

**数据结构变更**：
```c
typedef struct {
    char mac_address[MAC_ADDR_LEN + 1];  // MAC地址字符串
    char item_name[128];                 // ⭐ 新增：物品名称
    char storage_area;                   // ⭐ 新增：存放区域（A-Z）
    uint32_t quantity;                   // ⭐ 新增：物品数量
    float front_feature[FEATURE_VEC_SIZE]; // 正面特征向量
    float side_feature[FEATURE_VEC_SIZE];  // 侧面特征向量
    float top_feature[FEATURE_VEC_SIZE];   // 顶部特征向量
    bool is_valid;                          // 记录是否有效
} asset_record_t;
```

**向后兼容**：
- 旧格式检测：文件大小 = `OLD_RECORD_SIZE`
- 自动迁移：旧数据加载时填充默认值（item_name=""，storage_area='?'，quantity=0）
- 新格式保存：完整保存所有字段

---

### 3. 🔀 拍摄与推理线程分离

**功能描述**：将JPEG捕获（拍摄线程）和MobileNet推理（推理线程）分离到两个独立的FreeRTOS任务中，通过队列异步通信。

**核心价值**：
- ✅ **响应速度提升**：拍摄完成立即反馈，无需等待推理（~200ms vs ~7.5s）
- ✅ **用户体验优化**：拍摄后可立即进行下一步操作
- ✅ **并发处理能力**：拍摄和推理可并行执行
- ✅ **系统稳定性增强**：避免长时间阻塞导致看门狗超时
- ✅ **资源利用优化**：CPU和PSRAM负载更均衡

**架构设计**：
```
┌─────────────────┐         ┌──────────────────┐
│  camera_ai_task │         │ inference_task   │
│  (拍摄线程)      │         │ (推理线程)        │
│                 │         │                  │
│ • JPEG捕获      │────┐    │ • 接收推理任务    │
│ • 图片保存      │    │    │ • 多帧采集(3帧)   │
│ • 入队推理任务  │    │    │ • MobileNet推理   │
│ • 立即反馈      │    │    │ • 特征融合        │
│                 │    │    │ • 更新全局特征    │
└─────────────────┘    │    │ • 完成后触发      │
                       │    └──────────────────┘
                       ▼
              ┌──────────────────┐
              │ xInferenceQueue  │
              │ (推理任务队列)    │
              │                  │
              │ inference_job_t  │
              │ - view_cmd       │
              │ - mac            │
              │ - expected_views │
              │ - is_registration│
              └──────────────────┘
```

**工作流程**：
```
1. 用户输入 'f' (拍摄正面)
   ↓
2. camera_ai_task:
   - 加锁捕获JPEG (~200ms)
   - 保存图片到TF卡
   - 解锁摄像头
   - 创建inference_job_t并入队
   - 立即返回："Front view captured"
   ↓
3. inference_task (后台异步):
   - 从队列接收任务
   - 循环3次：
     * 加锁捕获RGB帧
     * MobileNet推理
     * 添加到融合缓冲区
     * 解锁
   - 计算平均特征
   - L2归一化
   - 存入g_front_feature
   - g_views_processed++
   ↓
4. 检查是否全部完成：
   if (g_views_processed == g_total_views):
     发送 CMD_INFERENCE_TRIGGER
   ↓
5. camera_ai_task 收到触发：
   - 注册模式：保存资产
   - 盘点模式：开始分析
   - 出库模式：比对正视图
```

**关键代码**：
```c
// 推理任务结构体
typedef struct {
    system_cmd_t view_cmd;          // CMD_CAPTURE_FRONT / SIDE / TOP
    char mac[MAC_ADDR_LEN + 1];     // MAC地址
    int expected_views;             // 期望的总视图数 (注册/盘点=3, 出库=1)
    bool is_registration;           // 注册模式(true: 需保存JPEG, false: 盘点/出库)
    bool must_save_jpeg;            // 是否必须保存JPEG(注册模式)
} inference_job_t;

// 进度计数器
int g_views_enqueued = 0;   // 已入队推理任务数
int g_views_processed = 0;  // 已完成推理数
int g_total_views = 0;      // 期望总视图数
```

**性能对比**：
| 指标 | V2.4（单线程） | V2.5（双线程） | 提升 |
|------|---------------|-----------------|------|
| 拍摄反馈延迟 | ~7.5秒 | ~200ms | **37倍** |
| 用户感知等待 | 每次拍摄都等待 | 仅最后等待分析 | **体验大幅提升** |
| 看门狗风险 | 高（长时间阻塞） | 低（及时复位） | **稳定性增强** |
| CPU利用率 | 串行执行 | 部分并行 | **效率提升** |

---

## 📈 性能对比

| 指标 | V2.4 | V2.5 | 提升 |
|------|------|--------|------|
| **识别准确率** | >95% | >95% | 持平 |
| **单次拍摄反馈** | ~7.5秒 | ~200ms | **37倍** |
| **注册完整流程** | ~25秒 | ~25秒 | 持平（但体验更好） |
| **盘点完整流程** | ~25秒 | ~25秒 | 持平（但体验更好） |
| **出库完整流程** | ❌ 不支持 | ~7.5秒 | **新功能** |
| **资产信息管理** | 仅MAC | MAC+名称+区域+数量 | **功能增强** |
| **列表显示** | 仅MAC | 完整信息表格 | **易用性提升** |
| **线程架构** | 单线程 | 双线程异步 | **架构优化** |
| **系统稳定性** | ✓ 优秀 | ✓ 卓越 | **进一步增强** |

---

## 🔧 技术架构变更

### 新增模块

无新增模块文件，主要在现有模块中增强功能。

### 修改模块

1. **main.c** - 核心架构重构
   - 新增`inference_task()`推理线程
   - 新增`xInferenceQueue`推理任务队列
   - 修改`camera_ai_task()`仅负责JPEG捕获
   - 新增出库模式命令处理（`CMD_OUTBOUND_ANALYZE`、`CMD_OUTBOUND_UPDATE_QTY`）
   - 新增`CMD_INFERENCE_TRIGGER`处理逻辑
   - 新增全局变量：`g_is_outbound_mode`、`g_outbound_quantity`等

2. **cmd_handler.c** - 命令处理器增强
   - 新增出库模式状态机（`CAM_STATE_WAITING_OUT_MAC`、`CAM_STATE_WAITING_OUT_QTY`）
   - 修改注册流程：增加物品名称、存放区域、数量输入步骤
   - 修改列表显示：完整展示资产详细信息
   - 新增`show_registration_name_guide()`、`show_registration_area_guide()`、`show_registration_quantity_guide()`
   - 修改`handle_mac_initialization()`根据模式设置`g_total_views`

3. **asset_manager.c/h** - 资产管理扩展
   - 修改`asset_record_t`结构体：新增`item_name`、`storage_area`、`quantity`字段
   - 修改`asset_load()`：支持旧格式自动迁移
   - 修改`asset_list_uart()`：详细表格显示资产信息
   - 保持向后兼容性

4. **main.h** - 接口定义更新
   - 新增`CMD_OUTBOUND_ANALYZE`、`CMD_OUTBOUND_UPDATE_QTY`、`CMD_INFERENCE_TRIGGER`命令
   - 新增`CAM_STATE_WAITING_OUT_MAC`、`CAM_STATE_WAITING_OUT_QTY`、`CAM_STATE_READY_OUT`状态
   - 新增`inference_job_t`结构体定义
   - 新增全局变量声明：`g_is_outbound_mode`、`g_outbound_quantity`等
   - 新增推理进度计数器声明

---

## ⚠️ 重要注意事项

### 1. 出库模式使用规范
- ✅ 仅适用于已注册的资产
- ✅ 出库前务必核对MAC地址和物品信息
- ✅ 数量输入必须为正整数
- ⚠️ 出库数量不能超过当前库存
- ⚠️ 数量归零后资产将被自动删除（不可恢复）

### 2. 资产信息完整性
- ✅ 物品名称建议简洁明了（1-127字符）
- ✅ 存放区域必须是单个字母（A-Z）
- ✅ 数量必须大于0
- ⚠️ 旧版本注册的资产可能缺少这些信息（显示为空或默认值）
- ✅ 建议重新注册以获得完整信息

### 3. 双线程架构特性
- ✅ 拍摄后立即反馈，无需等待推理
- ✅ 推理在后台异步进行
- ✅ 全部视图推理完成后才触发最终操作
- ⚠️ 不要在推理过程中强制退出（可能导致状态不一致）
- ✅ 使用`exit`命令会清空推理队列并重置计数器

### 4. 向后兼容性
- ✅ 新版本可以读取旧版本的资产文件
- ✅ 旧格式会自动迁移到新格式（填充默认值）
- ⚠️ 新版本保存的文件无法被旧版本读取
- ✅ 建议在升级后备份重要资产数据

---

## 📚 文档更新

所有文档已同步更新至V2.5：

- ✅ **README.md** - 主项目说明（更新出库模式和双线程架构）
- ✅ **IMPLEMENTATION_SUMMARY.md** - 技术实现总结（更新架构说明）
- ✅ **USER_GUIDE.md** - 用户使用指南（新增出库模式使用说明）
- ✅ **RELEASE_NOTES_V2.4.md** - 重命名为RELEASE_NOTES_V2.5.md
- ✅ **DOCUMENT_UPDATE_LOG.md** - 文档更新日志（新增V2.5记录）

---

## 🚀 升级指南

### 从V2.4升级到V2.5

1. **备份现有代码和数据**
   ```bash
   git clone <repository_url>
   cd CAM_AI
   git checkout v2.4.0
   git tag backup_v2.4
   # 备份TF卡中的资产数据
   ```

2. **获取V2.5代码**
   ```bash
   git pull origin main
   git checkout v2.5
   ```

3. **清理并重新编译**
   ```bash
   idf.py fullclean
   idf.py build
   idf.py flash monitor
   ```

4. **测试新功能**
   - 测试出库模式完整流程
   - 验证资产详细信息注册
   - 检查列表显示的完整信息
   - 观察拍摄反馈速度提升
   - 验证旧资产文件的兼容性

5. **阅读新文档**
   - 查看README.md了解出库模式
   - 参考USER_GUIDE.md学习新的注册流程
   - 查阅IMPLEMENTATION_SUMMARY.md了解双线程架构

---

## 🙏 致谢

感谢所有为V2.5版本做出贡献的开发者和测试人员！

特别感谢：
- 提出出库管理需求的仓库管理员用户
- 建议添加资产详细信息的业务分析师
- 设计双线程架构的性能优化工程师
- 测试向后兼容性的QA团队

---

## 📞 技术支持

如有问题，请通过以下方式联系：

- 📧 Email: support@esp32-cam-ai.com
- 💬 GitHub Issues: https://github.com/your-repo/issues
- 📖 文档：README.md, USER_GUIDE.md, TROUBLESHOOTING.md

---

## 📅 未来计划

V2.5版本计划（预计2026-05-28）：

- [ ] 批量盘点模式（连续扫描多个资产）
- [ ] LCD显示屏支持（本地显示资产信息）
- [ ] 语音提示功能（TTS播报操作引导）
- [ ] 自适应融合帧数（根据置信度动态调整）
- [ ] 云端同步支持（WiFi上传资产数据）
- [ ] 出库历史记录（保存每次出库操作日志）

---

**发布日期**: 2026-04-28  
**版本号**: V2.5  
**维护团队**: ESP32-S3 CAM AI Team  
**许可证**: MIT