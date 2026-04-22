# ESP32-S3 CAM AI - MobileNetV2 快速开始指南

## ⚠️ 重要提示：ESP-IDF版本要求

**在开始之前，请确认你的ESP-IDF版本 >= 5.3.0**

检查当前版本：
```bash
idf.py --version
```

如果显示 v5.1.x 或更低版本，请先升级ESP-IDF。

查看升级指南: [UPGRADE_ESP_IDF.md](UPGRADE_ESP_IDF.md)

---

# 🚀 ESP32-S3 CAM AI - 5分钟快速上手

## 🚀 5分钟快速上手

### 前提条件
- ✅ ESP32-S3开发板（带PSRAM）
- ✅ OV5640摄像头模块
- ✅ ESP-IDF v5.3或更高版本
- ✅ USB数据线

### 步骤1: 编译和烧录

```bash
# 进入项目目录
cd CAM_AI

# 设置目标芯片
idf.py set-target esp32s3

# 配置项目（可选）
idf.py menuconfig

# 编译
idf.py build

# 烧录（替换PORT为你的串口设备）
idf.py flash monitor -p /dev/ttyUSB0
# Windows用户: idf.py flash monitor -p COM3
```

### 步骤2: 连接WiFi

1. 在手机或电脑上搜索WiFi热点
2. 连接到 `ESP32_CAM`（密码：`12345678`）

### 步骤3: 查看视频流

在浏览器中打开：
```
http://192.168.4.1/
```

你应该能看到实时摄像头画面！

### 步骤4: 测试MobileNetV2特征提取

#### 使用串口工具（如PuTTY、minicom或Arduino IDE串口监视器）

**配置**:
- 波特率: 115200
- 数据位: 8
- 停止位: 1
- 校验位: None

#### 测试流程

**第1步**: 发送命令 `1` 采集第一个样本
```
发送: 1
等待: ~1.3秒
预期输出: "MobileNetV2 Feature 1 stored"
```

**第2步**: 发送命令 `2` 采集第二个样本并比对
```
发送: 2
等待: ~1.3秒
预期输出: 
  - 相同物体: "Same object! MobileNetV2 Similarity: 0.XX"
  - 不同物体: "Different objects! MobileNetV2 Similarity: 0.XX"
```

**第3步**: 重置（可选）
```
发送: r
预期输出: "Feature reset"
```

### 💡 测试技巧

#### 获得最佳结果

1. **环境准备**:
   - 选择光线充足的室内环境
   - 避免强烈背光
   - 保持背景简洁

2. **拍摄同一物体**:
   ```
   命令1 → 拍摄苹果（正面）
   命令2 → 拍摄苹果（正面，相似角度）
   预期: Similarity > 0.85 ✅
   ```

3. **拍摄不同物体**:
   ```
   命令1 → 拍摄苹果
   命令2 → 拍摄香蕉
   预期: Similarity < 0.85 ✅
   ```

4. **测试鲁棒性**:
   ```
   命令1 → 拍摄杯子（距离30cm）
   命令2 → 拍摄杯子（距离40cm）
   预期: Similarity > 0.80（应该能识别为同一物体）
   ```

### 📊 理解相似度数值

| 相似度范围 | 含义 | 建议操作 |
|-----------|------|---------|
| 0.90 - 1.00 | 非常相似 | 几乎肯定是同一物体 |
| 0.80 - 0.90 | 高度相似 | 很可能是同一物体 |
| 0.70 - 0.80 | 中等相似 | 可能是同类物体 |
| 0.50 - 0.70 | 较低相似 | 可能不是同一物体 |
| < 0.50 | 不相似 | 几乎肯定不是同一物体 |

### 🔧 常见问题快速解决

#### Q1: 编译错误 "imagenet_cls.hpp not found"

**解决**:
```bash
# 重新生成IDE配置
idf.py reconfigure

# 或清理后重新编译
idf.py fullclean
idf.py build
```

#### Q2: 运行时 "Camera init failed"

**检查**:
1. 摄像头接线是否正确
2. 引脚配置是否匹配你的硬件
3. PSRAM是否启用

**验证PSRAM**:
```bash
idf.py menuconfig
→ Component config → ESP PSRAM → Enable SPI RAM
```

#### Q3: 命令无响应

**检查**:
1. 串口波特率是否为115200
2. 是否正确发送单个字符（不是字符串"1\n"）
3. 查看串口监视器是否有其他错误信息

#### Q4: 相似度总是很低

**优化**:
1. 确保两次拍摄的是完全相同的物体
2. 保持相似的拍摄角度和距离
3. 在稳定光照条件下测试
4. 尝试降低阈值到0.75测试

#### Q5: 推理速度太慢

**说明**: 
- ESP32-S3上1.3秒/帧是正常性能
- 这是深度学习模型的特性

**如需更快**:
- 升级到ESP32-P4（约350ms/帧）
- 或使用原有的手工特征方法（~150ms/帧）

### 📝 示例测试脚本

使用Python自动测试（需要pyserial库）:

```python
import serial
import time

# 打开串口（Windows用COM3，Linux用/dev/ttyUSB0）
ser = serial.Serial('COM3', 115200, timeout=2)
time.sleep(2)  # 等待连接

def send_command(cmd):
    """发送命令并读取响应"""
    ser.write(cmd.encode())
    time.sleep(3)  # 等待推理完成
    response = ser.read_all().decode('utf-8', errors='ignore')
    print(response)
    return response

print("=== MobileNetV2 特征提取测试 ===\n")

# 测试1: 采集第一个样本
print("请准备好第一个物体，按回车继续...")
input()
send_command('1')

# 测试2: 采集第二个样本（同一物体）
print("\n请准备好同一个物体（可稍改变角度），按回车继续...")
input()
response = send_command('2')

if 'Same object' in response:
    print("✅ 成功识别为同一物体！")
else:
    print("⚠️  未识别为同一物体，可能需要调整阈值")

# 清理
ser.close()
```

### 🎯 下一步

掌握了基本使用后，你可以：

1. **调整阈值**: 修改`main.c`中的`COSINE_THRESHOLD`
2. **集成到应用**: 将特征提取功能集成到你的项目中
3. **扩展功能**: 
   - 添加多特征存储
   - 实现1:N识别
   - 保存到数据库
4. **探索其他模型**: 
   - 人脸识别（human_face_recognition）
   - 物体检测（yolo26_detect）
   - 手势识别（hand_gesture_recognition）

### 📚 更多资源

- [完整文档](README.md)
- [ESP-DL官方文档](https://docs.espressif.com/projects/esp-dl/)
- [示例代码](components/esp-dl/examples/)

---

**祝你使用愉快！** 🎉

如有问题，请查看完整README.md或提交Issue。