# ESP32-S3 CAM AI - 编译速查卡

## 🚀 一键编译（Windows）

```bash
# 1. 双击运行
setup_env.bat

# 2. 然后执行
idf.py set-target esp32s3
idf.py build
idf.py flash monitor -p COM3
```

## 🔧 手动修复步骤

### Git所有权警告
```bash
git config --global --add safe.directory D:/Espressif/frameworks/esp-idf-v5.1.2
```

### 组件找不到错误
确认根目录`CMakeLists.txt`包含：
```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/esp-dl"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/models/imagenet_cls"
)
```

### 清理重建
```bash
idf.py fullclean
idf.py build
```

## ❌ 常见错误速查

| 错误信息 | 原因 | 解决方法 |
|---------|------|---------|
| `Failed to resolve component 'imagenet_cls'` | 组件路径未配置 | 检查EXTRA_COMPONENT_DIRS |
| `detected dubious ownership` | Git安全检测 | 运行setup_env.bat或忽略 |
| `does not contain a CMakeLists.txt` | 路径指向错误目录 | 确认是`esp-dl/esp-dl`不是`esp-dl` |
| `Camera init failed` | PSRAM未启用 | menuconfig → Enable SPI RAM |
| `头文件找不到` (IDE) | IDE索引滞后 | `idf.py reconfigure` |

## 📁 关键文件位置

```
CAM_AI/
├── CMakeLists.txt              ← 添加EXTRA_COMPONENT_DIRS
├── main/
│   └── CMakeLists.txt          ← 声明REQUIRES imagenet_cls
├── components/
│   └── esp-dl/
│       ├── esp-dl/             ← 主组件（有CMakeLists.txt）
│       └── models/
│           └── imagenet_cls/   ← 模型组件（有CMakeLists.txt）
└── setup_env.bat               ← 环境配置脚本
```

## 💡 提示

- ✅ 编译前运行`setup_env.bat`（Windows）
- ✅ 修改CMakeLists.txt后执行`idf.py fullclean`
- ✅ IDE报错但能编译 = 正常，执行`idf.py reconfigure`
- ⚠️ MobileNetV2需要PSRAM支持
- ⚠️ 首次编译较慢（下载依赖）

## 📞 需要帮助？

查看详细文档：
- [README.md](README.md) - 完整使用说明
- [QUICKSTART.md](QUICKSTART.md) - 5分钟快速上手
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - 详细故障排除