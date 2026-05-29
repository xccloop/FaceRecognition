# 人脸识别 — MobileFaceNet

## 在这个项目中的作用

MobileFaceNet 是项目中的人脸特征提取模型。将检测+对齐后的人脸图像（112×112）输入网络，输出一个 512 维的特征向量，用于人脸比对。

对应代码：`Linux/Model/src/featureextractor.cpp`

## 核心原理（用本项目代码理解）

### 1. 网络结构

MobileFaceNet 是 MobileNetV2 的变体，专为人脸识别优化。特点是轻量（参数少，适合边缘设备）、输出 512 维特征向量。

项目中使用 `w600k_mbf_opt` 模型（在 WebFace600k 数据集上训练，优化版）。

### 2. 特征提取流程

```cpp
// featureextractor.cpp — FeatureExtractor::extract()
ncnn::Mat in = ncnn::Mat::from_pixels(aligned_bgr.data, ncnn::Mat::PIXEL_BGR, 112, 112);
in.substract_mean_normalize(MEAN, NORM);   // 减均值/除标准差

ncnn::Mat out;
ncnn::Extractor ex = net_.create_extractor();
ex.input("input.1", in);
ex.extract("516", out);                    // 输出层名为 "516"

// 转为 std::vector
std::vector<float> feat(out.w);            // out.w = 512
for (int i = 0; i < out.w; i++) feat[i] = out[i];
```

### 3. L2 归一化

提取出的 512 维向量需要 L2 归一化，使其模长为 1。这样两个向量的点积就等于余弦相似度：

```cpp
// featureextractor.cpp — normalize()
void FeatureExtractor::normalize(std::vector<float>& feat) {
    float sum = 0.f;
    for (float v : feat) sum += v * v;
    float inv = 1.f / std::sqrt(sum + 1e-8f);  // L2 范数的倒数
    for (float& v : feat) v *= inv;              // 每个元素除以范数
}
```

数学上：归一化后 `||a|| = ||b|| = 1`，所以 `cos(a,b) = a · b`。

### 4. 余弦相似度匹配

```cpp
// featureextractor.cpp — cosineSimilarity()
float FeatureExtractor::cosineSimilarity(const std::vector<float>& a,
                                          const std::vector<float>& b) {
    float dot = 0.f;
    for (size_t i = 0; i < a.size(); i++) dot += a[i] * b[i];
    return dot;  // 因为在 normalize() 后，点积 = 余弦相似度
}
```

### 5. 识别决策

```cpp
// main.cpp — FeatureDB::identify()
Match identify(const std::vector<float>& feat, float threshold = 0.4f) {
    for (const auto& e : registry_) {
        float sim = cosineSimilarity(feat, e.feature);
        if (sim > best.confidence) best = {e.name, sim};
    }
    if (best.confidence < threshold) return {"", best.confidence};  // 低于阈值=陌生人
    return best;
}
```

- 阈值 0.4：两张同一人脸的特征相似度通常在 0.5~0.8，不同人 < 0.3
- 遍历注册库中所有人，找最大相似度，低于阈值判定为 UNKNOWN

## 要学到什么程度

- 理解人脸识别的基本原理：检测 → 对齐 → 特征提取 → 相似度比对
- 理解 L2 归一化的数学意义：让点积等于余弦相似度
- 理解阈值的作用：区分"认识的人"和"陌生人"
- 知道 MobileFaceNet 是为移动端/边缘设备优化的轻量网络
