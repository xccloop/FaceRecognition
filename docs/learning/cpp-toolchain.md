# C++ 编译工具链

## 在这个项目中的作用

树莓派上的推理程序（`facerec`）是纯 C++ 项目，通过 CMake 构建，依赖 ncnn 和 OpenCV 两个 C++ 库。

## 项目中用到的具体知识

### CMakeLists.txt 结构

```cmake
# Linux/Model/CMakeLists.txt
cmake_minimum_required(VERSION 3.14)
project(FaceRecognition VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)          # 使用 C++17 标准

# 找库：ncnn
set(NCNN_DIR "/usr/local")          # ncnn 默认安装路径
find_library(NCNN_LIB ncnn ...)     # 查找 libncnn.a / libncnn.so
# 找库：OpenCV
find_package(OpenCV REQUIRED)
# 找库：OpenMP（ncnn 依赖多线程）
find_package(OpenMP REQUIRED)

# 列出所有 .cpp 源文件
set(SOURCES src/facedetector.cpp src/facealigner.cpp ...)

# 生成可执行文件
add_executable(facerec ${SOURCES})
target_link_libraries(facerec PRIVATE ${OpenCV_LIBS} ${NCNN_LIB} OpenMP::OpenMP_CXX)
```

### 编译流程

```bash
cd Linux/Model
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4        # 4 核并行编译
```

### 关键概念

| 概念 | 说明 | 本项目中的体现 |
|------|------|----------------|
| 静态链接 | 库代码打包进可执行文件 | ncnn 默认编译为 `libncnn.a` |
| 动态链接 | 运行时加载 .so 文件 | OpenCV 通常是 `libopencv_core.so` |
| 头文件路径 | 编译时找 `.h` 的位置 | `-I/usr/local/include/ncnn` |
| 库路径 | 链接时找 `.a/.so` 的位置 | `-L/usr/local/lib -lncnn` |

### ncnn 在 Pi 上的安装路径

编译 ncnn 源码后，库文件安装到 `/usr/local/lib/`，头文件在 `/usr/local/include/ncnn/`。CMake 中通过 `NCNN_DIR` 变量指向 `/usr/local`。

## 要学到什么程度

- 能用 CMake 组织一个多文件 C++ 项目（知道 `add_executable`、`target_link_libraries` 的写法）
- 理解头文件、库文件、链接的关系（编译器怎么找到第三方库）
- 能在 Pi 上从源码编译安装一个 C++ 库（如 ncnn）
- 理解 make -j 并行编译的意思
