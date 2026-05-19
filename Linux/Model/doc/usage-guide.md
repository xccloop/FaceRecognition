# 使用指南

## 环境准备

```bash
# 创建 conda 环境
conda create -n facerec python=3.12 -y
conda activate facerec
pip install opencv-python onnxruntime numpy
```

---

## 一、注册人脸

拍一张正脸照（白底、光线好、脸占画面 1/3 以上），保存为 `my_face.jpg`：

```bash
python verify.py register my_face.jpg "我的名字"
```

输出：
```
Registering '我的名字' from my_face.jpg (1120x2676)...
  Face: bbox=[226,1171,904,2014] score=0.7271
  Saved: features/我的名字.bin
  Registered!
```

> **注意**：检测分（score）应 > 0.5，人脸至少 300 像素宽。低于此标准的注册照会导致识别准确率下降。

---

## 二、照片识别

```bash
python verify.py identify test.jpg
```

输出：
```
Identifying from test.jpg (1120x2676)...
  Registry: ['xzc']
  Face: bbox=[191,975,1075,1994] score=0.5989
    vs xzc: similarity=0.6547 <- MATCH
  >>> IDENTIFIED: xzc (confidence: 0.6547)
```

- **similarity > 0.4** → 同一人（MATCH）
- **similarity ≤ 0.4** → 陌生人（NO MATCH）

---

## 三、1:1 比对

```bash
python verify.py compare img1.jpg img2.jpg
```

输出：
```
Comparing img1.jpg vs img2.jpg...
  img1.jpg: score=0.7271
  img2.jpg: score=0.7390
  Similarity: 0.6071
  Result: SAME person
```

---

## 四、实时摄像头

```bash
python verify.py live
```

界面显示：
- 人脸框 + 5 个关键点（黄点）
- 已注册者：绿框 + 名字 + 置信度
- 未注册者：橙框 + "?" + 分数
- 左上角 FPS
- 按 **Q** 退出

**摄像头命令**：
```bash
# 先注册
python verify.py register my_face.jpg xzc

# 启动摄像头
python verify.py live
```

---

## 五、管理注册库

注册特征存储在 `features/` 目录：

```bash
# 查看已注册的人
ls features/

# 删除某人
rm features/某名字.bin
```

---

## 六、模型文件目录

```
models/
├── onnx_models/buffalo_sc/  ← ONNX 源模型（推理用）
│   ├── det_500m.onnx         ← SCRFD 人脸检测
│   └── w600k_mbf.onnx        ← MobileFaceNet 特征提取
└── ncnn_models/              ← ncnn 模型（树莓派 C++ 用，需修复）
    ├── det_500m.param / .bin
    └── w600k_mbf.param / .bin
```

---

## 常见问题

**Q: 摄像头打不开？**
换摄像头索引：`cv2.VideoCapture(1)`（在 verify.py 的 live 模式中修改）。

**Q: 注册照检测不到人脸？**
- 确认照片是正脸、光线充足
- 降低阈值（verify.py 中 `score_thresh` 从 0.3 降到 0.1）
- 换白底背景、用后置摄像头拍摄

**Q: 同一人的两张照片匹配不上？**
注册照质量不过关。重新在白墙前拍摄，确保人脸清晰、占画面一半以上。
