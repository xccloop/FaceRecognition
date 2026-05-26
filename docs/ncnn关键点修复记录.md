# ncnn SCRFD 检测器关键点偏移问题修复记录

## 问题

C++ ncnn 版本的人脸检测输出 5 个关键点（左眼、右眼、鼻尖、左嘴角、右嘴角）与五官不对齐，而 Python ONNX 版本 (`verify.py`) 输出正常。关键点偏移导致后续人脸对齐错误 → 特征向量偏离 → 识别精度下降。

## 根因

**ncnn 模型检测头输出通道排列与 C++ 解码逻辑不匹配。**

### ncnn 模型结构

`det_500m_dyn.param` 中每个 FPN 层（stride 8/16/32）的检测头：

```
Split → 三条分支:
  ├─ Conv(2)  → Permute → Reshape(0=1)  → Sigmoid → blob {443,468,493} (score)
  ├─ Conv(8)  → Permute → Reshape(0=4)  → blob {446,471,496} (bbox)
  └─ Conv(20) → Permute → Reshape(0=10) → blob {449,474,499} (kps)
```

Conv 的输出通道数是 PyTorch/ONNX 标准：
- score: 2 通道 = 2 anchors × 1 score
- bbox: 8 通道 = 2 anchors × 4 坐标 (dx,dy,dw,dh)
- kps: 20 通道 = 2 anchors × 5 关键点 × 2 坐标

PyTorch Conv2d 输出是 **anchor-major** 排列：

```
kps 通道 0-9   = anchor 0 的 10 个值 (kp0_x, kp0_y, ..., kp4_x, kp4_y)
kps 通道 10-19 = anchor 1 的 10 个值
```

### Reshape 如何改变布局

ncnn 的 `Reshape(0=10)` 把 20 通道压为 10 通道，每通道空间维度翻倍。ncnn 按内存顺序合并相邻通道：

```
通道 0 = [anchor0_kp0_x (H×W个), anchor0_kp0_y (H×W个)]
通道 1 = [anchor0_kp1_x,          anchor0_kp1_y        ]
通道 2 = [anchor0_kp2_x,          anchor0_kp2_y        ]
通道 3 = [anchor0_kp3_x,          anchor0_kp3_y        ]
通道 4 = [anchor0_kp4_x,          anchor0_kp4_y        ]
通道 5 = [anchor1_kp0_x,          anchor1_kp0_y        ]
通道 6 = [anchor1_kp1_x,          anchor1_kp1_y        ]
通道 7 = [anchor1_kp2_x,          anchor1_kp2_y        ]
通道 8 = [anchor1_kp3_x,          anchor1_kp3_y        ]
通道 9 = [anchor1_kp4_x,          anchor1_kp4_y        ]
```

Bbox 同理（`Reshape(0=4)`）：

```
通道 0 = [anchor0_dx, anchor0_dy]
通道 1 = [anchor0_dw, anchor0_dh]
通道 2 = [anchor1_dx, anchor1_dy]
通道 3 = [anchor1_dw, anchor1_dh]
```

Score 不受影响（`Reshape(0=1)` 合并 2 通道后直接是平铺的 `[a0_all, a1_all]`，与 anchor 迭代顺序一致）。

### 旧 C++ 解码（错误）

```cpp
// 假设通道 p*2 包含所有 anchor 的 kp_p_x，通道 p*2+1 包含所有 anchor 的 kp_p_y
kdp[(p * 2 + 0) * N_total + k]  // 读 kp_p_x
kdp[(p * 2 + 1) * N_total + k]  // 读 kp_p_y
```

这导致 `kdp[1]` 实际读到的是 anchor0 的 kp1_x（通道 1），而不是 anchor0 的 kp0_y（通道 0 后半）。关键点坐标被错误通道的数据替换。

Bbox 同理：`bdp[1]` 读到 anchor0 的 dw，而非 dy。

### 为什么 Python 正确

ONNX Runtime 直接运行 ONNX 模型，模型的 Reshape 已经正确地将输出整理为 `[N, 10]` 格式（每行 = 一个 anchor 位置的 10 个关键点值），`kp[i][p*2]` 和 `kp[i][p*2+1]` 对应同一位置的 kp_p_x 和 kp_p_y，不存在通道错位。

## 修复

`facedetector.cpp:100-137`。引入两个变量区分 anchor 编号和空间位置：

```cpp
int fm_w = (img_w + stride - 1) / stride;
int fm_h = (img_h + stride - 1) / stride;
int N_spatial = fm_h * fm_w;  // 每个 anchor 的空间位置数

int sub = k / N_spatial;      // anchor 编号 (0 或 1)
int pos = k % N_spatial;      // 空间位置 (y*W + x)
```

**Bbox 解码**（修复 dx,dy,dw,dh 对齐）：

```cpp
float bx = bdp[(sub * 2 + 0) * N_total + pos];            // dx: 通道 sub*2, 前半
float by = bdp[(sub * 2 + 0) * N_total + pos + N_spatial]; // dy: 通道 sub*2, 后半
float bw = bdp[(sub * 2 + 1) * N_total + pos];            // dw: 通道 sub*2+1, 前半
float bh = bdp[(sub * 2 + 1) * N_total + pos + N_spatial]; // dh: 通道 sub*2+1, 后半
```

**关键点解码**（修复 5 个关键点对齐）：

```cpp
for (int p = 0; p < 5; p++) {
    f.keypoints[p][0] = a.cx + kdp[(sub * 5 + p) * N_total + pos] * stride;           // x: 前半
    f.keypoints[p][1] = a.cy + kdp[(sub * 5 + p) * N_total + pos + N_spatial] * stride; // y: 后半
}
```

## 诊断过程

1. **排除硬件因素**：确认与摄像头分辨率无关（320×240 和 640×480 均有此问题）
2. **对照实验**：Python ONNX 版 (`verify.py`) 关键点正确，C++ ncnn 版错误 → 问题在 ncnn 链路
3. **排除预处理差异**：padding/max_side 逻辑与 Python 一致，坐标映射公式正确
4. **阅读 ncnn param 文件**：追溯检测头 Conv → Permute → Reshape 操作链，发现通道数变化 (20→10, 8→4)
5. **推断通道合并逻辑**：ncnn Reshape 按内存顺序合并相邻通道，得出每通道 = [单 anchor 的 x 值拼接, 同 anchor 同关键点的 y 值]
6. **验证猜想**：C++ 代码访问 `kdp[(p*2+1)]` 实际落到通道 1（anchor0_kp1_x），而非期望的 anchor0_kp0_y
7. **修复验证**：修改后编译通过，live 模式关键点对齐五官

## 相关文件

- `src/facedetector.cpp` — 修复位置
- `models/ncnn_models/det_500m_dyn.param` — 模型结构参考
- `verify.py` — Python ONNX 参考实现
