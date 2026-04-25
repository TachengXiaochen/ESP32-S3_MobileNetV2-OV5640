# ESP32-S3 CAM AI 快速参考卡 v2.3.0

## 📌 常用命令速查

### 🔑 MAC地址管理
```
AA:BB:CC:DD:EE:FF    # 输入MAC地址（必须先输入才能使用摄像头）
r                    # 重置系统，重新输入MAC
```

**注意**：系统在启动时自动初始化TF卡，无需手动切换存储模式。

### 📷 资产注册（三视图拍摄）
```
f                    # 拍摄正面视图 (Front)
s                    # 拍摄侧面视图 (Side)
t                    # 拍摄顶部视图并保存 (Top)
```
**顺序要求：** f → s → t（必须按此顺序）

**覆盖提示**：
- 首次注册：`Asset saved to SD card successfully.`
- 覆盖更新：`Asset UPDATED (overwritten) on SD card.`

### 🔍 智能盘点（引导式）
```
c                    # 进入盘点模式（自动引导拍摄三视图）
```

**盘点结果示例**：
```
========== INVENTORY RESULT ==========
  Front: 92.56 (×0.5)
  Side:  89.29 (×0.3)
  Top:   95.12 (×0.2)
  ----------------------------------------
  Weighted Confidence: 91.8745
  Threshold: 0.75
  ✅ MATCH - Same Asset
  MAC: AA:BB:CC:DD:EE:FF
========================================
```

**匹配判断**：
- ✅ **置信度 ≥ 0.75** → 确认为同一物品
- ❌ **置信度 < 0.75** → 不是同一物品

### 📋 其他功能
```
l                    # 列出所有已注册资产 + TF卡空间统计
i                    # 查看系统信息（堆内存、SDK版本等）
help / ?             # 显示帮助信息
```

---

## 🔄 典型工作流程

### 场景1：注册新资产
```
1. r                     ← 选择注册模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入MAC地址
   （系统自动初始化TF卡和摄像头）

3. f                     ← 拍摄正面
   （将物品正面对准摄像头）

4. s                     ← 拍摄侧面
   （将物品旋转90度）

5. t                     ← 拍摄顶部并保存
   
✅ 完成！资产已保存到TF卡
   首次注册：Asset saved to SD card successfully.
   覆盖更新：Asset UPDATED (overwritten) on SD card.
```

### 场景2：智能盘点（推荐）⭐
```
1. c                     ← 选择盘点模式
   （系统显示主菜单后输入）

2. AA:BB:CC:DD:EE:FF     ← 输入要盘点的MAC地址

3. 系统自动引导：
   [STEP 1/3] Please capture FRONT view
              Send 'f' to capture
   
   [STEP 2/3] Please capture SIDE view
              Send 's' to capture
   
   [STEP 3/3] Please capture TOP view
              Send 't' to capture and analyze

4. 自动输出分析报告：
   Weighted Confidence: 91.8745
   Threshold: 0.75
   ✅ MATCH - Same Asset

✅ 完成！系统自动判断是否为同一物品
```

### 场景3：查看已注册资产和存储空间
```
l                        ← 列出所有资产

显示:
=== Registered Assets ===
  [1] MAC: AA:BB:CC:DD:EE:FF
  [2] MAC: 11:22:33:44:55:66
Total: 2 assets

=== Storage Statistics ===
  Total Space: 7.5 GB
  Used Space:  150 KB
  Free Space:  7.5 GB
  Usage:       0.00%
  Status:      ✓ Healthy
========================
```

---

## ⚠️ 注意事项

### MAC地址格式
- ✅ 正确: `AA:BB:CC:DD:EE:FF`
- ❌ 错误: `AABBCCDDEEFF`（缺少冒号）
- ❌ 错误: `aa:bb:cc:dd:ee:ff`（建议大写）

### 拍摄提示
- 💡 保持稳定光照
- 📏 距离约30-50cm
- 🎯 物品居中
- 🖼️ 背景简洁

### 匹配判断说明
- **阈值**: 0.75（加权置信度）
- **权重分配**: 正面50%, 侧面30%, 顶部20%
- **可调参数**: 修改代码中的 `MATCH_THRESHOLD` 常量

### 常见错误
```
"Invalid MAC format"        → 检查MAC地址格式
"Please capture FRONT view first"  → 必须按顺序拍摄
"Storage initialization failed"    → 检查TF卡是否正确插入
"SD card is FULL"                  → 清理部分资产或更换更大容量TF卡
```

---

## 🔧 故障排除

| 问题 | 解决方法 |
|------|----------|
| 摄像头不启动 | 先输入有效的MAC地址 |
| TF卡初始化失败 | 检查TF卡是否插入，格式是否为FAT32 |
| 相似度始终很低 | 检查光照和拍摄角度是否一致 |
| 系统重启 | MobileNetV2推理耗时较长属正常现象 |
| TF卡空间不足 | 使用 `l` 查看后删除部分资产，或更换更大容量TF卡 |

---

## 📊 性能指标

- **MobileNetV2推理**: ~2.5秒/次
- **三视图注册总耗时**: ~8-10秒
- **完整盘点流程**: ~10秒（含三视图采集+分析）
- **每个资产存储**: ~15KB
- **匹配阈值**: 0.75（可调整）
- **TF卡写入速度**: ~500KB/s
- **8GB卡容量**: 约50万个资产（理论最大值）

---

## 📁 文件存储

```
/sdcard/assets/
├── AA_BB_CC_DD_EE_FF.dat    # 资产特征数据（三个1280维向量）
└── ...
```

**单文件大小**: ~15KB  
**文件名格式**: MAC地址中的 `:` 替换为 `_`，后缀 `.dat`

---

**详细文档**: [README.md](README.md) | [USER_GUIDE.md](USER_GUIDE.md)  
**快速入门**: [QUICKSTART.md](QUICKSTART.md)  
**技术实现**: [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)

**版本**: v2.3.0 (2026-04-25)
