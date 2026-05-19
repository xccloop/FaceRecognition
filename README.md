# 四端协同人脸识别系统

基于树莓派 4B 的实时人脸识别系统，四端协同工作：手机采集人脸 → Windows 后台管理 → 树莓派实时推理 → STM32 执行动作。

## 系统架构

```
手机(小程序) ──拍照上传──► Windows后台 ──特征下发──► 树莓派4B ──UART──► STM32
                               ▲                        │                │
                               │                        ▼                ▼
                               └──────── 识别记录 ◄── 摄像头推理      LED/继电器/蜂鸣器
```

## 各端职责

| 端 | 职责 | 技术栈 | 状态 |
|----|------|--------|------|
| **树莓派 4B** | 核心推理引擎：摄像头采集 → SCRFD人脸检测 → 对齐 → MobileFaceNet特征提取 → 特征库比对 → UART输出结果 | C++ / ncnn / OpenCV | 🟡 推理管线完成，实时流水线待组装 |
| **STM32F103** | 接收识别结果，执行硬件动作（LED/继电器），心跳超时检测 | C / FreeRTOS / FWlib | 🟡 UART基础通信完成，协议待升级 |
| **Windows 后台** | 接收手机注册请求，提取人脸特征，通过SSH下发到树莓派 | Python / FastAPI / SQLite | ⬜ 待开发 |
| **手机小程序** | 拍照/选图，填写姓名，上传到Windows后台注册 | 微信小程序 | ⬜ 待开发 |

- 🟢 完成  &nbsp; 🟡 部分完成  &nbsp; ⬜ 待开发

## AI 模型管线

```
摄像头帧 ──► SCRFD (det_500m) ──► bbox + 5关键点 ──► 仿射变换 ──► 112×112 正脸
                                                                          │
                                                                          ▼
                                                               MobileFaceNet (w600k_mbf)
                                                                          │
                                                                          ▼
                                                                    512维特征向量
                                                                          │
                                                                          ▼
                                                                  特征库余弦比对
                                                                          │
                                                          ┌───────┬───────┴───────┬───────┐
                                                          │       │               │       │
                                                       匹配成功  检测到未识别     无人脸   多人脸
                                                       (0x10)    (0x11)         (0x12)   (0x13)
```

| 步骤 | 模型 | 输入 | 输出 | 推理框架 |
|------|------|------|------|----------|
| 人脸检测 | SCRFD det_500m | 任意尺寸 BGR | bbox + 5关键点 | ncnn |
| 人脸对齐 | OpenCV estimateAffinePartial2D | 5关键点 + 原图 | 112×112 正脸 | — |
| 特征提取 | MobileFaceNet w600k_mbf | 112×112 BGR | 512维归一化向量 | ncnn |

## 目录结构

```
FaceRecognition/
│
├── Linux/                         # 树莓派端
│   ├── Linux scheme.md            # 设计文档
│   ├── doc/
│   │   ├── tasks.md               # 待完成事项
│   │   └── stduy/study.md         # 树莓派开发学习笔记
│   └── Model/
│       ├── CMakeLists.txt         # C++ 构建
│       ├── verify.py              # Python 验证脚本（可用）
│       ├── inc/                   # C++ 头文件
│       │   ├── facedetector.h
│       │   ├── facealigner.h
│       │   ├── featureextractor.h
│       │   └── custom_layers.h
│       ├── src/                   # C++ 实现
│       │   ├── main.cpp           # CLI 工具 (register/identify/compare)
│       │   ├── facedetector.cpp   # SCRFD 检测 + NMS
│       │   ├── facealigner.cpp    # 5点仿射变换
│       │   ├── featureextractor.cpp # MobileFaceNet 提取
│       │   └── custom_layers.cpp  # Shape/Gather 自定义层
│       ├── models/                # 模型文件
│       │   ├── det_500m_simp.onnx # SCRFD ONNX 模型（~2.5MB）
│       │   └── ncnn_models/       # ncnn 转换模型
│       ├── features/              # 注册的人脸特征 .bin
│       └── doc/                   # 模型/推理/问题排查文档
│
├── Stm32/                         # STM32 端
│   ├── Project.uvprojx            # Keil 工程
│   ├── Transmit.txt               # 通信协议说明
│   ├── User/
│   │   ├── main.c                 # FreeRTOS + UART 接收任务
│   │   ├── stm32f10x_it.c         # USART2 ISR
│   │   └── stm32f10x_it.h
│   ├── FreeRTOS/                  # FreeRTOS 源码
│   ├── Fwlib/                     # STM32 标准外设库
│   └── doc/
│       └── tasks.md               # 待完成事项
│
├── Windows/                       # Windows 后台
│   └── doc/
│       └── tasks.md               # 待完成事项 + 技术方案
│
├── Phone/                         # 手机小程序
│   └── doc/
│       └── tasks.md               # 待完成事项 + 技术方案
│
├── 项目总体方案.md                  # 项目总方案文档
├── FaceRecongnition_description.txt # 原始项目描述
└── README.md
```

## 当前进度

### 树莓派端

**已完成：**
- ncnn + OpenCV 环境搭建与 CMake 构建
- SCRFD 人脸检测：anchor生成、多尺度解码、NMS 后处理
- 5点仿射变换人脸对齐（ArcFace 112×112 标准模板）
- MobileFaceNet 特征提取 + L2归一化 + 余弦相似度比对
- CLI 工具：register（注册人脸）、identify（识别是谁）、compare（比对两张脸）
- Python 版验证脚本（verify.py）：支持实时摄像头，已验证识别精度（同人 sim≈0.6，陌生人 sim≈0.15）

**待完成：** 摄像头采集线程、特征库 FeatureDB、UART 协议编解码、串口收发线程、config.json、主程序线程调度、systemd 自启

**已知问题：** C++ ncnn 版 SCRFD 自定义层（Shape/Gather）pass-through 实现不准确，导致推理结果为空。Python onnxruntime 版功能完整可用。

### STM32 端

**已完成：**
- FreeRTOS 运行正常（静态内存分配）
- USART2 初始化 + RXNE 中断接收
- 字节接收队列 + 基础 8 字节帧解析状态机
- PA0 LED 状态指示

**待完成：** 替换为完整变长帧协议（含转义）、命令分发 handler、心跳超时检测、UART 发送函数、看门狗

### Windows 后台 — 待开发
### 手机小程序 — 待开发

## 快速开始

### Python 验证（PC，推荐先跑）

```bash
pip install opencv-python onnxruntime numpy
cd Linux\Model

# 下载 ONNX 模型到 models/onnx_models/buffalo_sc/
# (从 InsightFace 下载 buffalo_sc.zip 解压)

# 注册人脸
python verify.py register my_face.jpg 张三

# 识别
python verify.py identify test.jpg

# 实时摄像头
python verify.py live
```

### C++ 编译（树莓派 / 有 ncnn 环境的 PC）

```bash
# 安装依赖
sudo apt install -y build-essential cmake libopencv-dev
# 编译 ncnn（树莓派上）
git clone https://github.com/Tencent/ncnn.git
cd ncnn && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DNCNN_BUILD_EXAMPLES=OFF ..
make -j2 && sudo make install

# 编译本项目
cd Linux/Model
cmake -B build && cmake --build build -j4
./build/facerec test my_face.jpg
```

### STM32 编译

用 Keil MDK-ARM 打开 `Stm32/Project.uvprojx`，编译下载。

## UART 帧协议

```
┌──────┬──────┬──────┬──────────┬──────┬──────┬──────┐
│ 帧头  │ 命令  │ 方向  │ 数据长度  │ 数据  │ 校验  │ 帧尾  │
│ 0xAA │ CMD  │ DIR  │ 2B(H-L)  │ 变长  │ XOR  │ 0x55 │
└──────┴──────┴──────┴──────────┴──────┴──────┴──────┘

方向: 0x01=Pi→STM32, 0x02=STM32→Pi
校验: 字段逐字节异或
转义: 0xAA/0x55/0xBB 特殊处理

Pi→STM32:  0x10 识别成功  0x11 未识别  0x12 无人脸  0x1F 心跳
STM32→Pi:  0x20 开门确认  0x21 注册    0x22 删除    0x23 查询
```

## 开发顺序

| 阶段 | 内容 | 状态 |
|------|------|------|
| 第一阶段 | 树莓派推理管线 + STM32 UART 协议联调 | 🟡 进行中 |
| 第二阶段 | Windows 后台 + 树莓派网络通信 | ⬜ |
| 第三阶段 | 手机小程序 + 全系统联调 | ⬜ |

## 硬件

| 设备 | 规格 |
|------|------|
| 树莓派 | 4B, 2GB RAM, Raspberry Pi OS Lite 64-bit |
| 摄像头 | USB 或 CSI, 建议 320×240 |
| STM32 | STM32F103C8T6 (Cortex-M3) |
| 连接 | 树莓派 GPIO14/15 ↔ STM32 PA3/PA2 (UART 115200) |
