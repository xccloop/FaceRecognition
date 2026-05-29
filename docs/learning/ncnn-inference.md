# ncnn 推理框架

## 在这个项目中的作用

ncnn 是树莓派上的推理引擎，负责加载人脸检测和人脸识别两个神经网络模型，对摄像头画面进行前向推理。

## 项目中用到的具体知识

### 三个核心概念

| 概念 | 类型 | 作用 |
|------|------|------|
| `ncnn::Net` | 网络对象 | 加载模型文件，管理网络结构 |
| `ncnn::Extractor` | 推理器 | 执行一次前向推理：喂输入、取输出 |
| `ncnn::Mat` | 数据容器 | 存放输入图像或输出特征向量 |

### 模型加载

```cpp
// Linux/Model/src/facedetector.cpp
ncnn::Net net_;
net_.opt.use_vulkan_compute = false;   // Pi 没有 GPU，禁用 Vulkan
net_.opt.num_threads = 2;              // 用 2 个 ARM CPU 核心
net_.load_param("det_500m_dyn.param");  // 加载网络结构（层定义）
net_.load_model("det_500m_dyn.bin");    // 加载权重数据
```

- `.param` 文件：描述网络结构（有哪些层、每层的参数）
- `.bin` 文件：存储所有权重数值

### 推理流程

```cpp
// 1. 图像 → ncnn::Mat（BGR → 减均值/除方差）
ncnn::Mat in = ncnn::Mat::from_pixels(img.data, ncnn::Mat::PIXEL_BGR, w, h);
in.substract_mean_normalize(mean_vals, norm_vals);

// 2. 创建推理器
ncnn::Extractor ex = net_.create_extractor();

// 3. 设置输入（"input.1" 是模型第一层的名字）
ex.input("input.1", in);

// 4. 执行推理并取输出
ncnn::Mat out;
ex.extract("516", out);   // "516" 是输出层的名字

// 5. 读取结果
std::vector<float> feat(out.w);
for (int i = 0; i < out.w; i++) feat[i] = out[i];
```

### 输入输出层名字

不同模型的层名字不同，需要查看原模型的导出信息：
- 检测模型输入：`"input.1"`，输出：`"443"`, `"468"`, `"493"`（3 个尺度的分数）、`"446"`, `"471"`, `"496"`（边界框）、`"449"`, `"474"`, `"499"`（关键点）
- 识别模型输入：`"input.1"`，输出：`"516"`

### ARM NEON 加速

ncnn 在 ARM 平台上自动使用 NEON SIMD 指令集加速卷积运算。`num_threads = 2` 利用了 Pi 4B 的多核。

## 要学到什么程度

- 理解 `param`（结构）和 `bin`（权重）两个文件的作用
- 理解推理的基本流程：加载 → 创建 Extractor → input → extract → 读取输出
- 知道 ncnn 是为移动端/边缘设备优化的，不依赖 GPU，纯 CPU 推理
- 能看模型文档知道输入输出层的名字
