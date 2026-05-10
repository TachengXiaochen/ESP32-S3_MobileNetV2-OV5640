# ESP32-S3 CAM AI - 编译速查卡（V2.6）

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
| `模糊度检测误判过多` ⭐NEW V2.6 | 阈值设置不当 | 调整blur_detection.h中的默认阈值 |

## 📁 关键文件位置

```
CAM_AI/
├── CMakeLists.txt              ← 添加EXTRA_COMPONENT_DIRS
├── main/
│   ├── blur_detection.c/h      ← V2.6新增：模糊度检测模块
│   ├── mobilenet_wrapper.cpp   ← 集成模糊度检测
│   └── ...
```

## 🔍 V2.6新增功能验证

编译成功后，可通过以下方式验证模糊度检测功能：

1. **查看启动日志**：
   ```
   [I] blur_detection: Blur detection module initialized
   ```

2. **拍摄时观察日志**：
   ```
   [I] mobilenet_wrapper: Checking image sharpness...
   [I] mobilenet_wrapper: Image is sharp enough for feature extraction
   ```

3. **测试模糊图像过滤**：
   - 故意快速移动摄像头拍摄
   - 应看到"Image is too blurry, discarding frame"提示
   - 系统会自动重拍下一帧

## 📊 V2.6性能指标

- **编译时间**: ~2-3分钟（首次编译）
- **固件大小**: ~2.5MB（含模糊度检测模块）
- **内存占用**: +230KB峰值（临时缓冲区）
- **准确率提升**: +3-5%

---

**文档版本**: V2.6  
**最后更新**: 2026-04-29
