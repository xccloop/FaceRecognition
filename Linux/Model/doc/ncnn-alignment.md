# C++/ncnn 与 Python/ONNX 识别精度对齐

本文档记录如何将 C++/ncnn 管线的人脸识别精度对齐到 Python/ONNX 的水平。

---

## 背景

在修复前，同一张 `my_face.jpg` 分别通过 Python+ONNX 和 C+++ncnn 提取特征，余弦相似度仅 **0.14**（≈ 随机）。经过系统性排查和修复，两套管线现已达到同等识别精度。

---

## 根因分析（按影响排序）

### 根因 1：检测预处理不一致（最主要）

| | C++ ncnn（修复前） | Python ONNX |
|---|---|---|
| 缩放策略 | min-side：`min(640/w, 480/h)` | max-side：`480 / max(w, h)` |
| 输入尺寸 | 固定 640×480，灰边填充 | 动态尺寸，不填充 |
| 填充比例 | 最多 25% 是灰边 | 0% |
| 填充值 | (114, 114, 114) | 无填充 |

对于 1920×1080 照片：
- Python：scale=0.25 → 480×270，**100% 有效像素**
- C++（修复前）：scale=0.333 → 640×360 + 各 60px 灰边 → **仅 75% 有效像素**

灰边在 FPN 特征融合时产生非零激活，污染了 neck 层的特征图，导致检测框和关键点偏离。

### 根因 2：ncnn 模型 Interp 层硬编码输出尺寸

`det_500m.param` 中两个 Interp 层硬编码了输出尺寸：

```
Interp Resize_108: output_height=30 output_width=40   # 对应 480×640 输入的 stride-16 特征图
Interp Resize_128: output_height=60 output_width=80   # 对应 480×640 输入的 stride-8 特征图
```

这导致模型**只能接受 640×480 的固定输入**，无法像 ONNX 模型那样处理任意尺寸。

### 根因 3：检测阈值和多人脸策略差异

| | C++（修复前） | Python |
|---|---|---|
| register/identify 阈值 | 0.7 | 0.3 |
| compare 阈值 | 0.5 | 0.3 |
| 多人脸处理 | 严格报错退出 | 取最高分人脸 |

高阈值导致同人在不同光照/角度下可能根本检不出脸。多人脸时 C++ 直接报错而非降级处理。

---

## 修复方案

### 修复 1：Interp 层改用比例缩放（`det_500m_dyn.param`）

将硬编码输出尺寸改为 2× 比例因子：

```
# 修复前
Interp Resize_108: height_scale=1.0 width_scale=1.0 output_height=30 output_width=40
Interp Resize_128: height_scale=1.0 width_scale=1.0 output_height=60 output_width=80

# 修复后
Interp Resize_108: height_scale=2.0 width_scale=2.0 output_height=0 output_width=0
Interp Resize_128: height_scale=2.0 width_scale=2.0 output_height=0 output_width=0
```

`output_height=0 output_width=0` 让 ncnn 在推理时根据实际输入尺寸动态计算输出尺寸，等效于 ONNX 模型的动态 Interp 行为。

这要求输入尺寸为 32 的倍数（确保 FPN 各层特征图尺寸在 2× 上采样时严格对齐）。代码中通过最小填充（≤31px）保证此约束。

### 修复 2：预处理对齐 Python

```cpp
// 修复前：min-side 缩放 + 大范围灰边填充
float scale = min(640.0f / orig_w, 480.0f / orig_h);  // 可能 != Python 的 scale
// pad to 640×480 with (114,114,114)

// 修复后：max-side 缩放 + 最小对齐填充
float scale = 1.0f;
if (max(orig_w, orig_h) > 480)
    scale = 480.0f / max(orig_w, orig_h);  // 与 Python 完全一致
// pad to multiple of 32 with (128,128,128) → 归一化后 ≈ 0
```

关键改动：
- 缩放因子与 Python 完全一致
- 填充从固定 640×480（最多 210px）缩小到 32 的倍数对齐（最多 31px）
- 填充值从 114 改为 128，使归一化后 `(128-127.5)/128 ≈ 0`，网络感知为"中性"

### 修复 3：统一阈值和多人脸策略

- 所有模式检测阈值统一为 0.3（匹配 Python）
- 多人脸时选最高分而非报错

```cpp
// 修复前
if (faces.size() > 1) throw error;

// 修复后
if (faces.size() > 1) {
    sort by score desc;
    use faces[0];  // 最高分
}
```

---

## 验证方法

修复后应重新注册人脸并对比：

```bash
# 重新注册（旧特征库已不兼容）
./facerec.exe register my_face.jpg test_user

# 同一人识别应 > 0.4
./facerec.exe identify same_person_another_photo.jpg

# 比对同一人应 > 0.4
./facerec.exe compare photo1.jpg photo2.jpg
```

**预期结果**：同人相似度 > 0.4（通常 0.5~0.7），陌生人 < 0.3。

---

## 残留差异

以下差异不显著影响识别精度，但值得注意：

| 项目 | C++ ncnn | Python ONNX |
|------|----------|-------------|
| 推理引擎 | ncnn (CPU) | onnxruntime (CPU) |
| 数值精度 | FP32 | FP32 |
| NMS 实现 | 迭代贪心 | 向量化 `any()` |
| 检测模型 | det_500m_dyn (.param 手工修改) | det_500m.onnx (原生动态) |
| 最小填充 | 32 的倍数对齐（≤31px） | 无填充 |

核心的检测缩放、特征提取归一化、对齐参考点已完全一致。
