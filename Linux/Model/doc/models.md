# 模型方案与获取

## 方案设计

人脸识别拆分为三步，全部通过 ncnn 在树莓派 4B 2G 上运行：

| 步骤 | 模型/方法 | 输入 | 输出 |
|------|----------|------|------|
| ① 检测 | SCRFD (det_500m) | 任意尺寸图像 | bbox + 5关键点 |
| ② 对齐 | OpenCV 仿射变换 | 原图 + 5关键点 | 112×112 正脸 |
| ③ 特征 | MobileFaceNet (w600k_mbf) | 112×112 图像 | 512维向量 |

---

## 模型获取

InsightFace 官方发布在 GitHub Release，base URL：
```
https://github.com/deepinsight/insightface/releases/download/v0.7/
```

用 `buffalo_sc.zip`（15MB），包含所需两个模型。单独 .onnx 文件不出现在 Release 中，必须下载 zip 包解压。

**环境**：conda env `facerec` (python=3.12)，装 `requests tqdm onnx onnx-simplifier`。

### 模型规格

**det_500m.onnx**: 输入 `[1,3,H,W]`，输出 9 个头（3 stride × 3 类），关键点 `[N,10]` = 5点×2坐标，146 算子 opset 11。

**w600k_mbf.onnx**: 输入 `[1,3,112,112]`，输出 `[1,512]`，98 算子 opset 11。

---

## ONNX → ncnn 转换

### 工具链

ncnn 20260113 已移除 onnx2ncnn，用 20241226 版本（最后一个含 onnx2ncnn.exe）：
```
https://github.com/Tencent/ncnn/releases/download/20241226/ncnn-20241226-windows-vs2022.zip
```

### 转换流程

```
ONNX → onnxsim(简化) → onnx2ncnn(.param+.bin) → ncnnoptimize(算子融合)
```

- det_500m: 动态输入导致 onnxsim 失败，跳过简化直接用原模型转换
- w600k_mbf: onnxsim 成功，batch 维度自动设为 1
- ncnnoptimize: det_500m 融合 Conv+ReLU，w600k_mbf 融合 Gemm+BN

### 转换结果

```
ncnn_models/
├── det_500m_dyn.param / .bin  ← SCRFD（Interp 动态化，推荐使用）
├── det_500m.param / .bin      ← SCRFD（原始转换，仅适配 640×480）
└── w600k_mbf.param / .bin     ← MobileFaceNet（Gemm+BN 已融合）
```

> **注意**：`det_500m.param` 的 Interp 层硬编码了 640×480 对应的输出尺寸，仅支持固定尺寸输入。`det_500m_dyn.param` 将 Interp 改为 2× 比例缩放（`output_height=0 output_width=0`），支持任意尺寸输入，与 Python ONNX 管线行为一致。详见 `doc/ncnn-alignment.md`。

---

## 性能优化

按投入产出比排序：

**第一优先：INT8 量化** — 2~4× 加速，精度损失 < 0.5%。用 ncnn2table + ncnnoptimize 量化，需 100~500 张校准图片。

**第二优先：降低检测分辨率** — 640→480px 省一半耗时，320px 再减半但远处小人脸漏检。

**第三优先：多线程** — 检测开 2 线程，特征提取 1 线程，不占满 4 核。

**不需要做：Vulkan GPU** — 树莓派上 CPU 推理比 Vulkan 快（模型太小，GPU 调度开销 > 收益）。

**流程级优化**（视频场景）：跳帧检测（3~5 帧检一次）、ROI 跟踪、特征缓存。

### 预期性能（树莓派 4B）

| 配置 | 耗时 | FPS |
|------|------|-----|
| 未优化 (FP32, 640px) | 140~250ms | 4~7 |
| INT8 + 480px + 双线程 | 50~80ms | 12~20 |

---

## 模型替代选项

若需更小模型：
- 检测：SCRFD 320M / 160M
- 识别：ShuffleFaceNet (1.3× 速度) / VarGFaceNet (1.5× 速度, 320维)
