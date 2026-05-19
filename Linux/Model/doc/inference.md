# 推理代码：设计、编译与验证

## 架构

```
输入图像
  │
  ├── FaceDetector::detect()    → SCRFD 前向推理 → anchor 解码 → NMS → bbox + 5关键点
  ├── FaceAligner::align()      → 5点仿射变换 → 112×112 正脸
  └── FeatureExtractor::extract() → MobileFaceNet 推理 → L2 归一化 → 512维特征
```

---

## 双轨实现

| 实现 | 语言 | 推理引擎 | 状态 | 用途 |
|------|------|----------|------|------|
| `src/*.cpp` | C++ | ncnn | 🟡 待修复 | 树莓派最终部署（高性能） |
| `verify.py` | Python | onnxruntime | 🟢 可用 | PC 验证 + 实时摄像头 |

C++ 版当前受限于 ncnn 自定义层（Shape/Gather）转换不完全，见 `troubleshooting.md` 问题 6-7。Python 版用 onnxruntime 原生支持所有 ONNX 算子，无需自定义层。

**树莓派部署路径**：
1. 最简单：Python + onnxruntime（RPi 上 ~2-4 FPS）
2. 中等：C++ + onnxruntime C++ API（~5-10 FPS）
3. 最快：等 ncnn 转换问题解决后，C++ + ncnn（~10-20 FPS）

---

## 运行模式（verify.py）

```
verify.py register <photo.jpg> <name>   # 注册人脸
verify.py identify <photo.jpg>          # 识别是谁
verify.py compare <img1.jpg> <img2.jpg> # 比对两张脸
verify.py live                          # 实时摄像头
```

---

## 核心设计

### SCRFD 检测 (vectorized decode)

```python
# 缩放至 480px max → 检测 → bbox/kps 缩放回原图
scale = 480 / max(h, w)
img_small = cv2.resize(img, (int(w*scale), int(h*scale)))

# 向量化 anchor 解码（替代 Python 三重循环）
cand_idx = np.where(scores > threshold)[0]
top500 = np.argpartition(-scores, 500)[:500]
cy = (cand_idx // (fm_w*2)) * stride + 0.5*stride
cx = ((cand_idx // 2) % fm_w) * stride + 0.5*stride
x1 = max(0, cx - bbox[:,0]*stride)  # bbox[:,0] = left distance
```

**关键点**：
- 3 个 stride [8,16,32]，小脸/中脸/大脸
- 每位置 2 anchor，FCOS 距离编码 `[left,top,right,bottom]`
- 中心坐标 `(j+0.5)×s` 位于 cell 中心，非角点
- 每个 stride 只取 top-500 候选 → NMS

### 5 点对齐

ArcFace 标准 112×112 模板：
```
左眼(30.3,51.7) 右眼(65.5,51.7) 鼻子(48.0,71.7)
左嘴角(33.5,92.4) 右嘴角(62.7,92.4)
```
`cv::estimateAffinePartial2D` 5 点最小二乘，比 3 点精确匹配对噪声更鲁棒。

### 特征提取

- 归一化：`(img - 127.5) / 127.5` → [-1, 1]（与 SCRFD 的 1/128 不同！）
- L2 归一化后点积 = 余弦相似度
- 识别阈值：sim > 0.4 = 同一人（已验证：同人 0.6+，陌生人 0.15）

### 摄像头实时优化

| 优化 | 方法 | 效果 |
|------|------|------|
| 缩放检测 | 480px max，bbox/kps 缩放回原图 | 3-5× |
| 向量化解码 | numpy 替代 Python 循环 | 5-10× |
| 跳帧检测 | 每 5 帧检测一次 | 1.7× |
| 特征缓存 | IOU 跟踪，人脸不动时复用特征 | 5× |
| 候选限制 | 每 stride top-500 pre-NMS | 2× |
| 静默日志 | `ort.set_default_logger_severity(3)` | 省 I/O |

PC 上 20-30 FPS，树莓派预估 5-8 FPS。

---

## 验证结果

使用高质量白底注册照（人脸 678×843px, score=0.73）：

| 测试 | 人脸大小 | 检测分 | 相似度 | 判定 |
|------|----------|--------|--------|------|
| test1 (同一人，不同角度) | 622×777 | 0.74 | **0.61** | ✅ 正确识别 |
| test3 (同一人，另一角度) | 884×1019 | 0.60 | **0.65** | ✅ 正确识别 |
| test2 (陌生人) | 423×564 | 0.83 | **0.15** | ✅ 正确拒绝 |

**关键经验**：注册照人脸至少 300 像素宽，检测分 > 0.5，否则特征质量不足以跨照片匹配。

---

## 编译

### Windows（C++ ncnn 版）

需 VS Build Tools 2022 + OpenCV conda：
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
      -DOpenCV_DIR="D:/ANACONDA/Library/cmake/x64/vc17/lib"
cmake --build build --config Release
```
运行前：`set PATH=%PATH%;D:\ANACONDA\Library\bin`

### 树莓派（C++ ncnn 版）

```bash
sudo apt install -y build-essential cmake git libopencv-dev
# 编译 ncnn
git clone https://github.com/Tencent/ncnn.git
cd ncnn && git submodule update --init && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=OFF -DNCNN_BUILD_EXAMPLES=OFF ..
make -j4 && sudo make install
# 本项目
cd Model && cmake -B build && cmake --build build -j4
./build/facerec test.jpg
```

### Python 版（跨平台，直接运行）

```bash
pip install opencv-python onnxruntime numpy
python verify.py live
```

---

## 项目结构

```
Model/
├── CMakeLists.txt              # C++ 构建
├── verify.py                   # Python 验证 + 实时摄像头
├── inc/                        # C++ 头文件
│   ├── facedetector.h / facealigner.h / featureextractor.h / custom_layers.h
├── src/                        # C++ 实现
│   ├── main.cpp / facedetector.cpp / facealigner.cpp / featureextractor.cpp
│   └── custom_layers.cpp
├── models/
│   ├── onnx_models/buffalo_sc/ # ONNX 源模型
│   └── ncnn_models/            # ncnn 转换模型
├── features/                   # 注册的人脸特征
└── doc/
    ├── models.md               # 模型方案 & 性能优化
    ├── inference.md            # 本文档
    └── troubleshooting.md      # 11 个问题排查
```
