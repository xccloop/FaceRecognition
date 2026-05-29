# 人脸检测 — SCRFD

## 在这个项目中的作用

SCRFD（Sample and Computation Redistribution for Efficient Face Detection）是项目中的人脸检测模型。输入一张摄像头画面，输出画面中所有人脸的边界框（bbox）和 5 个关键点（双眼、鼻尖、两个嘴角）。

## 项目中使用的具体模型

`det_500m_dyn` — SCRFD 500M 参数的动态版本，在 Pi 上每帧检测耗时约 80-120ms（320×240 输入）。

对应代码：`Linux/Model/src/facedetector.cpp`

## 核心原理（用本项目代码理解）

### 1. Anchor-Free 检测

不预设锚框（anchor box），而是把图像划分成网格，每个格点预测一个检测结果。本项目中用 3 个 stride（8, 16, 32）覆盖不同尺度的脸：

```cpp
// facedetector.cpp
static const int STRIDES[3] = {8, 16, 32};  // 3 个特征层级

// 生成锚点：每个 feature map 位置一个中心点
void FaceDetector::generateAnchors(int w, int h) {
    for (int s : STRIDES) {
        int fm_w = (w + s - 1) / s;   // 该层特征图宽度
        int fm_h = (h + s - 1) / s;
        for (int sub = 0; sub < 2; sub++) {   // 每层 2 个子锚
            for (int i = 0; i < fm_h; i++) {
                float cy = (i + 0.5f) * s;    // 映射回原图坐标
                for (int j = 0; j < fm_w; j++) {
                    float cx = (j + 0.5f) * s;
                    anchors_.push_back({cx, cy, s});
                }
            }
        }
    }
}
```

- Stride 8：512/8=64，特征图 64×64，检测小脸
- Stride 16：512/16=32，特征图 32×32，检测中等脸
- Stride 32：512/32=16，特征图 16×16，检测大脸

### 2. 多尺度输出

3 个 stride 级别共输出 3 组张量，每组包含 score（置信度）、bbox（偏移量）、kps（关键点偏移）：

```cpp
const char* score_names[3] = {"443", "468", "493"};
const char* bbox_names[3]  = {"446", "471", "496"};
const char* kps_names[3]   = {"449", "474", "499"};
```

### 3. 预处理：缩放 + 32 对齐 + Padding

```cpp
// max-side 缩放（只缩小不放大）
float scale = (float)max_side / std::max(orig_w, orig_h);

// Pad 到 32 的倍数（FPN 要求特征图对齐）
int pw = ((nw + 31) / 32) * 32 - nw;
cv::copyMakeBorder(resized, img, pad_top, pad_bottom, pad_left, pad_right,
                   cv::BORDER_CONSTANT, cv::Scalar(128, 128, 128));
```

### 4. NMS（非极大值抑制）

同一张脸可能被多个锚点重复检测，NMS 按 score 排序后，逐对计算 IOU，重叠度高的只保留最优的那个：

```cpp
float iou = inter / (area_i + area_j - inter + 1e-6f);
if (iou > nms_thresh) suppressed[j] = true;  // 0.4 阈值
```

### 5. 边界过滤 + 坐标映射回原图

去掉检测框中心在 padding 区域外的假阳性，然后把坐标从缩放后的图像映射回原始尺寸：

```cpp
f.x1 = (f.x1 - pad_left) * inv;  // inv = 1.0 / scale
```

## 要学到什么程度

- 理解 stride 与感受野的关系：stride 越大，每个格点对应的原图区域越大，适合检测大目标
- 理解 NMS 的作用和 IOU 的计算方法
- 知道 `det_500m_dyn` 中的 `dyn` 表示动态输入尺寸（推理时可根据图像大小调整）
- 能从模型文档中理解输出层的含义（score/bbox/kps × 3 个 stride = 9 个输出张量）
