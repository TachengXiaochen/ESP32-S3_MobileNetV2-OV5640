# AI推理流水线速度优化方案

> 日期: 2026-06-06 | 版本: V3.4 | 状态: ✅ 已完成 | 实施报告: `docs/archive/20260606_INFERENCE_SPEED_OPTIMIZATION.md`

## 背景

当前系统拍摄三视图（前/侧/顶），每视图 3 帧 = 共 9 次 MobileNetV2 推理，耗时 **30-60 秒**，对实际应用（库存管理、出入库核验）太慢。目标是大幅提速且不损失识别质量。

### 核心瓶颈分析（每帧耗时拆解）

| 阶段 | 估计耗时 | 备注 |
|------|---------|------|
| 摄像头捕获 JPEG | ~50-100ms | `esp_camera_fb_get()` |
| 软件 JPEG 解码 | ~100-200ms | `sw_decode_jpeg` 320x240→RGB888 |
| 模糊检测 | ~20-40ms | 全分辨率 Laplacian 卷积 |
| MobileNetV2 INT8 推理 | ~2-3s | ESP-DL, 224×224 输入，最大瓶颈 |
| 反量化 + 温度缩放 | ~50-80ms | 1280 次 expf() + L2-norm |
| **人为延迟** | **800ms** | 4×200ms "TLSF 堆恢复" — 纯浪费 |
| 特征融合 | ~5ms | 平均 + BN + L2-norm |

**每帧总计 ~3-4s**，其中：
- 800ms 人为延迟是纯粹的无效等待
- 温度缩放/L2-norm 在 per-frame 阶段是冗余计算（融合后会再做一次归一化）

---

## 优化方案（分级实施）

### Tier 1：零质量风险，纯代码优化（预计节省 8-10 秒）

#### 1.1 移除 800ms 人为延迟 ⭐ 最高优先级

- **文件**: `main/modules/ai/mobilenet_wrapper.cpp` 第 246-250 行
- **当前代码**:
  ```cpp
  // 分段延迟,每200ms复位一次看门狗
  for (int i = 0; i < 4; i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_task_wdt_reset();
  }
  ```
- **改动**: 删除整个 for 循环块，替换为 `vTaskDelay(1)` 仅强制一次上下文切换
- **原因**: 这是早期堆损坏问题的 workaround。现有代码已在延迟前完成了所有内存释放：
  - `free(input_img.data)` — 第 224 行释放 JPEG 解码内存
  - `outputs.clear()` — 第 230 行清除模型输出引用
  - `esp_camera_fb_return(fb)` — 第 232 行归还摄像头帧缓冲
  - `heap_check` malloc/free — 第 236-239 行堆完整性探测
- **节省**: 9 帧 × 800ms = **7.2 秒**
- **风险**: 无

#### 1.2 移除每帧冗余的温度缩放

- **文件**: `main/modules/ai/mobilenet_wrapper.cpp` 第 199-211 行
- **当前代码**:
  ```cpp
  float *scaled_features = (float *)malloc(feat_len * sizeof(float));
  if (scaled_features && feature_processor_temperature_scaling(feature_vec, feat_len, 0.8f, scaled_features)) {
      memcpy(feature_vec, scaled_features, sizeof(float) * feat_len);
  }
  if (scaled_features) free(scaled_features);
  ```
- **改动**: 删除整个温度缩放代码块；如需保留效果，移到 `feature_processor_get_fused_feature()` 融合后执行一次
- **原因**: 温度缩放 = softmax(1280 维) + L2-norm。softmax 向量平均后再经 batch-norm + L2-norm（`feature_processor_get_fused_feature` 第 176-196 行），per-frame 的 softmax 被数学上抹平
- **节省**: 9 帧 × ~50ms + 9 次 malloc/free = **~0.5 秒**
- **风险**: 极低

#### 1.3 移除每帧冗余的 L2 归一化

- **文件**: `main/modules/ai/mobilenet_wrapper.cpp` 第 186-197 行
- **改动**: 删除 per-frame L2 归一化循环（两趟遍历 1280 floats）
- **原因**: `feature_processor_get_fused_feature()` 第 186-196 行已做 L2-norm，per-frame 归一化被后续平均和再归一化覆盖
- **潜在好处**: 保留原始特征幅度信息，让高置信度帧在平均中自然占更大权重
- **节省**: 极少量
- **风险**: 无

#### 1.4 模糊检测降采样 2×

- **文件**: `main/modules/ai/blur_detection.c`
- **改动**: 在 `blur_detect_laplacian_variance()` 中添加 stride=2 采样，有效分辨率从 320×240 降至 160×120
- **实现**: 修改内部 `rgb_to_gray` 和 `apply_laplacian`，每隔一行/列取一个像素，内存分配量也降为 1/4
- **原因**: 模糊是低频特性，半分辨率下的 Laplacian 方差与全分辨率相关性 >0.95
- **注意**: 模糊阈值从默认 50.0 按比例调整为 ~12.5（方差与有效像素数大致成比例）
- **节省**: 9 帧 × ~20ms = **~0.2 秒**
- **风险**: 极低

#### 1.5 尝试硬件 JPEG 解码

- **文件**: `main/modules/ai/mobilenet_wrapper.cpp` 第 89 行
- **改动**: `dl::image::sw_decode_jpeg` → `dl::image::hw_decode_jpeg`
- **原因**: ESP32-S3 有专用硬件 JPEG 解码器，DMA 传输不占 CPU
- **注意**: 需验证 ESP-DL 版本中 `hw_decode_jpeg` 的兼容性，回退方案为保持软件解码
- **节省**: 9 帧 × ~50-80ms = **~0.5-0.7 秒**
- **风险**: 低

---

### Tier 2：小架构调整（预计再节省 18-24 秒）

#### 2.1 帧数从 3 降为 1 + 自适应补充 ⭐ 最高收益

- **文件**: `main/main.c` (`inference_task`), `main/modules/ai/mobilenet_wrapper.h`, `main/modules/ai/blur_detection.h`
- **改动**:
  1. 默认每视图只采集 **1 帧**（`NUM_FRAMES = 1`）
  2. `mobilenet_extract_features()` 新增可选输出参数 `float *blur_score`（可传 NULL，向后兼容），返回当前帧的 Laplacian 方差值
  3. 在 `inference_task` 中加入自适应逻辑：

```
采集第1帧 → mobilenet_extract_features(&blur_score)
  │
  ├─ blur_score > BLUR_CONFIDENT (非常清晰)
  │     → 直接使用1帧 ✓
  │
  ├─ blur_score ∈ [BLUR_ACCEPT, BLUR_CONFIDENT] (边缘)
  │     → 补充再采2帧 → 3帧融合
  │
  └─ blur_score < BLUR_ACCEPT (模糊)
        → 已内部过滤返回false → 重试下一帧
```

- **阈值设计**:
  - 全分辨率 (320×240): `BLUR_ACCEPT = 50.0`, `BLUR_CONFIDENT = 150.0` (3× accept)
  - 半分辨率 (160×120, Tier 1.4 后): `BLUR_ACCEPT = 12.5`, `BLUR_CONFIDENT = 37.5`
- **分析**: 大多数正常拍摄场景下第 1 帧就足够清晰，直接使用 1 帧。只有少数边缘情况（环境抖动、对焦未稳）才触发多帧融合
- **节省**:
  - 正常情况: 推理次数从 9 → 3，~**18-24 秒**（67% 缩减）
  - 边缘情况: 推理次数从 9 → 最多 9（与当前持平，不更差）
- **质量保障**: 模糊检测阈值本身是可靠的质量门，方差远超阈值的帧无需多帧平均

#### 2.2 特征缓冲区复用

- **文件**: `main/modules/ai/feature_processor.c`, `main/modules/ai/feature_processor.h`
- **改动**: 新增 `feature_processor_reset_frame_count()` 函数，只重置 `frame_count = 0`，不释放/重分配 buffer
- **对应**: `main/main.c` 中的 `feature_processor_clear_buffer()` 替换为 `feature_processor_reset_frame_count()`
- **节省**: 每视图避免 3 × 1280 × 4 = 15KB 的 malloc/free 周期，减少堆碎片
- **注意**: `feature_processor_clear_buffer()` 保留不删除（其他地方可能调用），新函数作为更轻量的替代

---

### Tier 3：模型级优化（📋 未来扩展备选方案，本次不实施）

> 以下方案需要重新训练模型并在 ESP-DL 外部完成训练→量化→部署流程，风险较高，作为后续迭代方向保留。

#### 3.1 定制训练的可行性

当前使用的 `imagenet_cls_mobilenetv2_s8_v1.espdl` 是 ImageNet 1000 类预训练分类器，代码提取 softmax 前的 1280 维特征做相似度匹配（迁移学习 via 特征提取）。ES-DL **只做推理不做训练**，定制训练需走外部流程：

```
PyTorch 训练/微调 → 导出 ONNX → ESP-PPQ 量化 → .espdl 部署
```

具体步骤：
1. **PyTorch 训练**（PC端）：加载预训练 MobileNetV2，根据需求选择训练策略
2. **ESP-PPQ 量化**：`pip install esp-ppq` → `espdl_quantize_onnx(target="esp32s3")` → 输出 `.espdl`
3. **部署**：替换固件中的 `.espdl` 文件，调整 C++ 预处理参数

#### 3.2 可选定制方向

| 方向 | 速度提升 | 精度影响 | 工作量 | 说明 |
|------|---------|---------|-------|------|
| **度量学习 (Triplet Loss)** | 无 | **↑ 提升** | 中 | 同类靠近、异类远离，直接优化相似度匹配任务 |
| **蒸馏到 MobileNetV3-Small** | **2-3×** | 可能略降 | 中 | 小模型模仿当前大模型的 1280 维嵌入 |
| **降低输入 224→128** | **3-4×** | 略降 | 低 | 改分辨率重训，推理量降为 1/4 |
| **缩减嵌入 1280→256** | 轻微 | 略降 | 低 | 加投影层训练，减少存储和匹配开销 |
| **换用 YOLO 检测模型** | 视模型而定 | — | 高 | 如需要同时定位+识别，ESP-DL 的 YOLO26 支持 QAT 微调 |

#### 3.3 建议

积累一定量标注数据后再启动 Tier 3。优先方向：**度量学习提升精度** + **蒸馏到小模型提升速度** 的组合收益最大。

---

## 耦合度分析

所有改动仅在 AI 推理业务内部，**不影响其他模块**：

| 模块 | 改动性质 | 对外接口影响 |
|------|---------|------------|
| `mobilenet_wrapper.cpp` | 内部优化 | 仅新增可选参数 `float *blur_score`（传 NULL 行为不变） |
| `mobilenet_wrapper.h` | 新增可选参数 | 向后兼容 |
| `blur_detection.c` | 内部算法优化 | 对外接口 `blur_detect_is_sharp()` / `blur_detect_laplacian_variance()` 签名不变 |
| `blur_detection.h` | 新增阈值常量 | 纯增量，不修改已有声明 |
| `feature_processor.c` | 新增轻量函数 | 已有接口全部保留，新增 `reset_frame_count()` |
| `feature_processor.h` | 新增函数声明 | 纯增量 |
| `main.c` (inference_task) | 内部逻辑调整 | 队列接口 (`xInferenceQueue`/`xSystemQueue`) 不变 |
| `camera_module.c` | **不改动** | `capture_and_process` / `capture_jpeg` 签名不变 |
| `ai_module.c` | **不改动** | `match_features` / `calculate_confidence` 不变 |
| `similarity_matcher.c` | **不改动** | 匹配逻辑不变 |
| `business_executor.c` | **不改动** | 消息路由不变 |
| `uart_handler_*.c` | **不改动** | 通信协议不变 |

**结论**: 改动集中在 AI 推理流水线的 3 个环节（特征提取 → 模糊检测 → 帧融合），上下游接口保持兼容。

---

## 预期效果

| 阶段 | 帧/视图 | 总推理次数 | 预计总时间 | 缩减比例 |
|------|---------|-----------|-----------|---------|
| 当前 | 3 | 9 | 30-60s | 基准 |
| Tier 1 完成 | 1-3 (自适应) | 3-9 | ~18-42s | ~30% |
| Tier 1+2 完成 (正常) | 1 | 3 | **~5-10s** | **~80-85%** |
| Tier 1+2 完成 (边缘) | 3 | 9 | ~18-42s | 与当前持平 |

**目标**: 正常场景 3 视图全流程 **5-10 秒**，从当前 30-60 秒降低 80-85%。边缘场景不劣于当前。

---

## 质量保障计划

对每个 Tier 变更后：

1. 采集 10+ 已知物品（覆盖电子/家具/工具/容器各类别），建立 baseline 特征向量
2. 应用优化后重新采集相同物品
3. 计算：
   - 同物品优化前后的余弦相似度（应 >0.95）
   - 同物品 vs 不同物品的区分度比例（应保持不变）
4. 确认各类别的动态阈值仍然适用：
   - 电子: 0.85 | 家具: 0.70 | 工具: 0.78 | 容器: 0.75 | 默认: 0.75
5. 若任何指标劣化 >2%，回滚该变更并调查

---

## 实施顺序

```
1. Tier 1.1 (移除800ms延迟)       ← 立即见效7秒，零风险
2. Tier 1.2 + 1.3 (移除冗余归一化)  ← 简化代码，少量节省
3. Tier 1.4 (模糊降采样)           ← 少量节省
4. Tier 2.2 (缓冲区复用)           ← 减少内存碎片，无副作用
5. Tier 2.1 (1帧+自适应补充)       ← 最大收益，需适配 blur_score 接口
6. Tier 1.5 (HW JPEG)             ← 兼容性验证后
7. [烧录测试 + 质量验证]           ← 确认 5-10s 目标达成
8. Tier 3 (模型定制训练)           ← 后续迭代，积累数据后启动
```

---

## 关键文件清单

| 文件 | 改动内容 | Tier |
|------|---------|------|
| `main/modules/ai/mobilenet_wrapper.cpp` | 移除延迟、温度缩放、L2-norm；新增 blur_score 输出；可选 HW JPEG | 1.1-1.5, 2.1 |
| `main/modules/ai/mobilenet_wrapper.h` | 函数签名新增 `float *blur_score` 可选参数 | 2.1 |
| `main/modules/ai/blur_detection.c` | 降采样模糊检测 (stride=2) | 1.4 |
| `main/modules/ai/blur_detection.h` | 新增 `BLUR_CONFIDENT_THRESHOLD` 常量 | 2.1 |
| `main/main.c` | `NUM_FRAMES=1` + 自适应帧数逻辑；替换 clear_buffer → reset_frame_count | 2.1, 2.2 |
| `main/modules/ai/feature_processor.c` | 新增 `feature_processor_reset_frame_count()` | 2.2 |
| `main/modules/ai/feature_processor.h` | 新函数声明 | 2.2 |

## 验证方法

1. 编译烧录: `idf.py build flash monitor`
2. 通过 UART CLI 发送 `f`/`s`/`t` 命令触发三视图采集
3. 观察串口日志中的 `[INF]` 标签，记录每视图耗时和 blur_score 值
4. 对比优化前后的匹配置信度和准确率
5. 故意拍摄模糊场景验证自适应补充逻辑是否触发
