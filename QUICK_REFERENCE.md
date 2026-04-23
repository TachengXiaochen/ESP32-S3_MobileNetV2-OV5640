# ESP32-S3 CAM AI 快速参考卡

## 📌 常用命令速查

### 🔑 MAC地址管理
```
AA:BB:CC:DD:EE:FF    # 输入MAC地址（必须先输入才能使用摄像头）
r                    # 重置系统，重新输入MAC
```

### 💾 存储模式切换
```
storage sd           # 切换到SD卡存储模式
storage flash        # 切换到内部Flash（SPIFFS）模式（**默认**）
storage status       # 查看当前存储模式
```

**注意**：系统默认使用 SPIFFS（内部Flash）存储，无需外部 SD 卡即可正常使用。

### 📷 资产注册（三视图拍摄）
```
f                    # 拍摄正面视图 (Front)
s                    # 拍摄侧面视图 (Side)
t                    # 拍摄顶部视图 (Top)
```
**顺序要求：** f → s → t（必须按此顺序）

### 🔍 资产盘点
```
c                    # 进入盘点模式
1                    # 比对正面视图
2                    # 比对侧面视图
3                    # 比对顶部视图
```

### 📡 WiFi控制
```
wifi on              # 开启视频流
wifi off             # 关闭视频流（默认状态）
```

### 📋 其他功能
```
l                    # 列出所有已注册资产
```

---

## 🔄 典型工作流程

### 场景1：注册新资产
```
1. AA:BB:CC:DD:EE:FF     ← 输入MAC地址
   （等待系统初始化SD卡和摄像头）

2. f                     ← 拍摄正面
   （将物品正面对准摄像头）

3. s                     ← 拍摄侧面
   （将物品旋转90度）

4. t                     ← 拍摄顶部
   （从上往下拍摄）
   
✅ 完成！资产已保存到SD卡
```

### 场景2：盘点资产
```
1. AA:BB:CC:DD:EE:FF     ← 输入要盘点的MAC地址

2. c                     ← 进入盘点模式

3. 1                     ← 比对正面
   显示: FRONT similarity: 0.9234 [MATCH]

4. 2                     ← 比对侧面
   显示: SIDE similarity: 0.8876 [MATCH]

5. 3                     ← 比对顶部
   显示: TOP similarity: 0.9012 [MATCH]

✅ 三个视角都匹配，确认是同一物品
```

### 场景3：查看已注册资产
```
l                        ← 列出所有资产

显示:
=== Registered Assets ===
  [1] MAC: AA:BB:CC:DD:EE:FF
  [2] MAC: 11:22:33:44:55:66
Total: 2 assets
========================
```

### 场景4：开启WiFi视频流
```
wifi on                  ← 开启视频流

浏览器访问: http://192.168.4.1/
WiFi密码: 12345678

wifi off                 ← 使用完毕后关闭
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

### 状态提示
- `[MATCH]` - 相似度 ≥ 0.85，匹配成功
- `[NO MATCH]` - 相似度 < 0.85，不匹配

### 常见错误
```
"Invalid MAC format"        → 检查MAC地址格式
"Please capture FRONT view first"  → 必须按顺序拍摄
"Registration not complete" → 必须完成三视图才能盘点
"SD card init FAILED"      → 检查SD卡是否正确插入
```

---

## 🔧 故障排除

| 问题 | 解决方法 |
|------|----------|
| 摄像头不启动 | 先输入有效的MAC地址 |
| SD卡初始化失败 | 检查SD卡是否插入，格式是否为FAT32 |
| 相似度始终很低 | 检查光照和拍摄角度是否一致 |
| 系统重启 | MobileNetV2推理耗时较长属正常现象 |
| WiFi无法连接 | 确认SSID和密码正确，最多4个客户端 |

---

## 📊 性能指标

- **MobileNetV2推理**: ~1.3秒/次
- **三视图注册总耗时**: ~4-5秒
- **单次盘点比对**: ~1.3秒
- **每个资产存储**: ~15KB
- **相似度阈值**: 0.85（可调整）

---

## 🌐 WiFi配置

- **SSID**: `ESP32_CAM`
- **密码**: `12345678`
- **IP地址**: `192.168.4.1`
- **最大连接数**: 4

---

## 📁 文件存储

```
/sdcard/assets/
├── AA_BB_CC_DD_EE_FF.dat    # 资产特征数据
└── ...
```

---

**详细文档**: [USER_GUIDE.md](USER_GUIDE.md)  
**实现总结**: [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)
