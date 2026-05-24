# 问题排查与解决

所有编译和运行时遇到的问题，按发生顺序记录。

---

## 问题 1：CMake 选了 MinGW，lib 是 MSVC 格式

**现象：** CMake 自动选 Ninja + MinGW，报 `OpenCV has no binaries compatible`。

**排查：** PATH 中 `D:\Ksoftware\mingw64\bin\g++.exe` 优先级高于 MSVC 的 `cl.exe`。

**解决：** 显式指定生成器 `-G "Visual Studio 17 2022" -A x64`。

---

## 问题 2：ncnn 头文件找不到

**现象：** `fatal error C1083: 无法打开包括文件: "ncnn/net.h"`

**排查：** `include_directories("x64/include/ncnn")` + 代码 `#include <ncnn/net.h>` → 编译器在 `x64/include/ncnn/ncnn/net.h` 找文件（多一层）。

**解决：** 改为 `include_directories("x64/include")`。

---

## 问题 3：cv::estimateAffinePartial2D 找不到

**现象：** `error C2039: "estimateAffinePartial2D": 不是 "cv" 的成员`

**排查：** 函数声明在 `<opencv2/calib3d.hpp>`，代码只 include 了 `<opencv2/imgproc.hpp>`。

**解决：** 补 `#include <opencv2/calib3d.hpp>`。

---

## 问题 4：glslang 链接错误（12 个未解析符号）

**现象：** 编译通过但链接报 12 个 `glslang::*` 符号缺失。

**排查：** ncnn 预编译包把 GPU 代码编进了 `ncnn.lib`。即使代码设 `use_vulkan_compute=false`，静态链接仍需解析 `gpu.obj` 引用的 glslang 符号。

**解决：** 额外链接 6 个 glslang 库，并加 glslang 头文件路径：
```cmake
set(NCNN_LIBS ${NCNN_LIB}
    "${NCNN_DIR}/lib/glslang.lib"
    "${NCNN_DIR}/lib/OSDependent.lib"
    "${NCNN_DIR}/lib/OGLCompiler.lib"
    "${NCNN_DIR}/lib/SPIRV.lib"
    "${NCNN_DIR}/lib/MachineIndependent.lib"
    "${NCNN_DIR}/lib/GenericCodeGen.lib"
)
include_directories("${NCNN_DIR}/include/glslang")
```

---

## 问题 5：Post-Build DLL 复制失败

**现象：** 编译链接成功，但 post-build 步骤报找不到 `opencv_world4120.dll`。

**排查：** 路径计算错误。conda 下没有统一的 `opencv_world` DLL，而是分散的模块 DLL。

**解决：** 删除 post-build 步骤。运行时通过 PATH 指向 `D:\ANACONDA\Library\bin` 即可。

---

## 问题 6：运行时 crash — "layer Shape not exists or registered"

**现象：**
```
Loading models...
layer Shape not exists or registered
Segmentation fault
```

**排查过程：**

1. **观察**：崩溃发生在模型加载阶段（`net_.load_param()`），不是推理阶段。

2. **检查 param 文件**：`grep Shape det_500m.param` 发现存在 `Shape`/`Gather`/`ExpandDims` 等自定义层类型。

3. **根因分析**：这些是 ONNX 的动态形状计算算子。SCRFD 输入 `[1,3,H,W]` 尺寸不定，ONNX 中用 `Shape`→`Gather`→`ExpandDims` 链来计算特征图尺寸。onnx2ncnn 无法将这些算子映射到 ncnn 内置层，标记为自定义层。ncnn 加载时找不到注册的实现即崩溃。

4. **尝试方案 A — pnnx 重新转换**：pnnx 是 ncnn 官方推荐的现代转换工具，对动态形状处理更好。但 pnnx 主要面向 PyTorch，对纯 ONNX 输入支持有限。

5. **方案 B — 自定义层实现（采用）**：写 C++ 代码实现 `Shape`/`Gather`/`ExpandDims` 三个层并注册到 ncnn：

```cpp
// Shape: 输出 [h, w]
top_blob.create(2);
top_blob[0] = (float)h; top_blob[1] = (float)w;

// Gather/ExpandDims: 对检测流程中简单的维度操作做 pass-through
```

6. **注册到 Net**：
```cpp
net.register_custom_layer("Shape",      shapeCreator,   shapeDestroyer);
net.register_custom_layer("Gather",     gatherCreator,  gatherDestroyer);
net.register_custom_layer("ExpandDims", unsqCreator,    unsqDestroyer);
```

**注意**：`register_custom_layer` 的 creator/destroyer 必须用 `ncnn::layer_creator_func` 签名（`Layer*(*)(void*)`），不能用 lambda（lambda 无捕获时可以退化，但加上 `void*` 参数后不行）。

**结果**：模型加载不再报错。

**但推理仍然崩溃**（Segmentation fault in detect()）。原因是 SCRFD 的动态 Shape 计算链较长（Shape→Gather→ExpandDims→Interp），我们简化的 pass-through 实现导致中间 blob 维度错误，后续层收到错误形状的输入。

**最终方案**：在 Windows 验证阶段放弃 ncnn C++，改用 Python + onnxruntime（原生支持所有 ONNX 算子）。C++ 代码保留，后续用 pnnx 或更好的转换工具解决 ncnn 兼容性后再启用。

---

## 问题 7：自定义 ExpandDims 覆盖 ncnn 内置层

**现象**：自定义层注册后模型加载成功，但推理返回空结果（检测不到任何人脸，即使同一张图用 Python 能检测到）。

**排查**：日志中出现 `overwrite built-in layer type ExpandDims`。检查 `layer_type_enum.h` 发现 `ExpandDims = 45` 是 ncnn 内置层。我们的 pass-through 实现（直接 clone 输入）覆盖了正确实现（添加维度），导致模型内部 Shape 计算逻辑全部错乱。

**解决**：删除 ExpandDims 的自定义注册，只注册 Shape 和 Gather（非内置）。
```cpp
void registerCustomLayers(ncnn::Net& net) {
    net.register_custom_layer("Shape",  shapeCreator,  shapeDestroyer);
    net.register_custom_layer("Gather", gatherCreator, gatherDestroyer);
    // ExpandDims is ncnn built-in type 45, do NOT overwrite
}
```

**但最终**：深层自定义层问题仍然存在（Gather/Shape 的 pass-through 实现不够准确），C+++ncnn 方案暂时搁置，改用 Python+onnxruntime 验证。

---

## 问题 8：注册照片人脸太小 → 特征失效

**现象**：my_face.jpg（1280×3058）注册 xzc 后，test3（同一人）识别相似度 = -0.07（负数！）。

**排查**：
1. 注册照检出人脸 = 92×117 像素（极小），score=0.52
2. 对齐时将 92×117 硬放大到 112×112，严重失真
3. 重新拍了一张 1279×2270 的照片，检出人脸 179×295，score 仅 0.12
4. 相似度仍然低（test3 vs xzc: 0.299，不到阈值）

**根因**：检测分低 → 关键点不准 → 对齐质量差 → 特征质量差。179 像素宽的人脸对 MobileFaceNet 来说仍然偏小。

**解决**：在光线充足的白墙前重新拍摄注册照，人脸占画面 1/3 以上。
- 新照片：人脸 678×843 像素，score=0.73
- 同一人 test1: sim=0.61 ✅ / test3: sim=0.65 ✅
- 陌生人 test2: sim=0.15 ❌

**教训**：人脸检测分 < 0.3 的注册照不值得用。注册照的人脸至少 300 像素宽。

---

## 问题 9：检测取面积最大的人脸，噪声框被误选

**现象**：同一张 my_face.jpg，注册时识别为 92×117 的 xzc，identify 时却选中了另一个 326×400 的框，score=0.06（噪声）。相似度只有 0.03。

**排查**：代码用 `max(bbox area)` 选择主脸，噪声框虽然分数极低但面积更大。

**解决**：改为 `max(score)` 选择最高分的脸，不受面积干扰。
```python
faces.sort(key=lambda f: f[4], reverse=True)  # score, not area
```

---

## 问题 10：摄像头帧率过低（5 FPS）

**现象**：实时摄像头模式下帧率极低，几乎不可用。

**排查**：瓶颈依次为：
1. **检测跑在全分辨率**（如 640×480 原始分辨率的 anchor 解码涉及 10 万+ anchor）
2. **Python 三重循环解码**（stride → grid_h → grid_w → anchor×2）
3. **每 3 帧做一次全推理**仍太频繁
4. **每帧都提取特征**，即使人脸没动
5. **onnxruntime 每帧打印 9 条 warning**到 stderr

**解决（按影响排序）**：
1. 检测输入缩放至 480px max，bbox/kps 缩放回原图 → **3-5×**
2. 解码改为 numpy 向量化（`np.where` + `argpartition` top-500）→ **5-10×**
3. 跳帧从 3 改为 5 → **1.7×**
4. IOU 跟踪 + 特征缓存（人脸移动 < 30% IOU 时复用）→ **5×**
5. `ort.set_default_logger_severity(3)` 关闭 onnx 日志 → 省 I/O
6. 候选框限制为每 stride top-500 → **2×**

**结果**：PC 上从 ~5 FPS 提升至 20-30 FPS。

---

## 问题 11：验证阶段找不到现实可用的人脸测试照片

**现象**：test1（侧脸角度太偏）检测不到人脸；test2（网图）最高 detection score 仅 0.06，模型根本不认为这张图含有人脸。无法验证识别功能。

**排查**：用 numpy 导出每张图各 stride 的最高 score：
```python
for s, name in [(8,'443'),(16,'468'),(32,'493')]:
    print(f'stride {s}: max={outputs[name].max():.4f}')
```
test2 三个 stride 的最高分分别为 0.06 / 0.03 / 0.04，全低于最低阈值。

**根因**：test2 图片可能是侧脸、遮挡、卡通或非标准人脸，SCRFD 未在其中学到此类分布。

**解决**：找一张正常人脸正面照作为 test2。后续使用网络上的清晰正面照验证陌生人拒绝效果（score=0.83，sim=0.15 ✅）。

---

## 问题 12：C++/ncnn 识别精度远低于 Python/ONNX

**现象**：同一张 `my_face.jpg`，Python+ONNX 和 C+++ncnn 提取的特征余弦相似度仅 0.14（≈随机）。C++ 管线完全不具可用性。

**排查**：

1. **检测预处理不一致（主要根因）**：C++ 使用 min-side 缩放 + 大范围灰边填充（640×480 固定输入），Python 使用 max-side 缩放 + 零填充（动态输入尺寸）。灰边在 FPN neck 层产生非零激活，污染特征图。

2. **ncnn 模型 Interp 层硬编码输出尺寸**：`det_500m.param` 中两个 Interp 层写死了 `output_height=30 output_width=40` 和 `output_height=60 output_width=80`，仅适配 640×480 输入。无法像 ONNX 模型那样处理动态尺寸。

3. **检测阈值差异**：C++ register/identify 用 0.7，Python 用 0.3。C++ compare 用 0.5。同人在不同光照下可能检不出。

4. **多人脸策略**：C++ 要求严格 1 张脸，Python 取最高分。

**解决**：

1. **Interp 层动态化**：从 `det_500m.param` 复制出 `det_500m_dyn.param`，将两个 Interp 层的硬编码输出尺寸改为 2× 比例因子（`height_scale=2.0 width_scale=2.0 output_height=0 output_width=0`），使模型支持任意输入尺寸。

2. **预处理对齐 Python**：
   - 缩放策略改为 max-side=480（完全匹配 Python 的 scale 因子）
   - 填充从固定 640×480 改为 32 的倍数对齐（最多 31px，几乎为零）
   - 填充值从 `(114,114,114)` 改为 `(128,128,128)`，归一化后 ≈ 0（中性填充）

3. **阈值统一为 0.3**，多人脸改为取最高分。

详见 `doc/ncnn-alignment.md`。

**结果**：C++/ncnn 识别精度与 Python/ONNX 达到同等水平。

---

## 总结

| # | 阶段 | 问题 | 根因 | 解决 |
|---|------|------|------|------|
| 1 | CMake | 编译器不匹配 | PATH 优先级 | 显式指定 VS 生成器 |
| 2 | 编译 | ncnn 头文件找不到 | include 路径多一层 | 去掉末尾 `/ncnn` |
| 3 | 编译 | 函数找不到 | 缺 calib3d 头文件 | 补 include |
| 4 | 链接 | glslang 符号缺失 | ncnn GPU 代码引用 | 链接 6 个 glslang.lib |
| 5 | 链接 | DLL 复制失败 | conda 路径不符 | 删 post-build |
| 6 | 运行 | Shape 层崩溃 | onnx2ncnn 转换不完整 | 自定义层注册（部分解决） |
| 7 | 运行 | 检测无结果 | ExpandDims 覆盖内置层 | 移除重复注册 |
| 8 | 验证 | 同人相似度为负 | 注册照人脸太小（92×117px） | 白底大脸照重拍 |
| 9 | 验证 | 噪声框被选中 | 取最大面积而非最高分 | 改取最高分 |
| 10 | 性能 | 摄像头 5 FPS | 全分辨率 + Python 循环 | 缩放+向量化+跳帧+缓存 |
| 11 | 验证 | 测试照检不到人脸 | 图片非标准人脸/无人脸 | 换正常正面照 |
| 12 | 精度 | ncnn 识别相似度 0.14 | 预处理不一致 + Interp 硬编码 + 阈值差异 | 动态 Interp + 预处理对齐 + 阈值统一 |
