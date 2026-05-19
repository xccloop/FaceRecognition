# 树莓派 4B 人脸识别方案（聚焦模型部署 + STM32 通信）

## 零、开发环境区分

项目涉及两台机器，环境不同：

```
Windows PC（模型准备）              树莓派 4B（运行部署）
─────────────────────              ──────────────────
Conda 环境: facerec                OS: Raspberry Pi OS Lite 64-bit
Python 3.x                        C++ (gcc/g++ 10+)
依赖: onnx, onnxsim, onnxruntime  依赖: OpenCV(apt), ncnn(源码编译)
用途:                             用途:
  - ONNX 模型下载                    - 摄像头采集
  - 模型简化 (onnxsim)              - AI 推理（C++ 调用 ncnn）
  - ONNX → ncnn 转换               - UART 与 STM32 通信
  - .param/.bin 输出到 Model/       - 读取 Model/ 中的 ncnn 模型
  - 模型精度验证 (onnxruntime)       - 特征库管理与比对
```

> **关键**: PC 端用 conda `facerec` 环境做模型转换，产物是 `Model/ncnn_models/*.param` + `*.bin`。这些文件是跨平台的，传到树莓派后由 C++ ncnn 加载推理。

## 一、硬件平台

| 项目 | 规格 |
|------|------|
| 主控 | 树莓派 4B, 2GB RAM, 四核 Cortex-A72 @ 1.5GHz |
| 系统 | Raspberry Pi OS Lite 64-bit (aarch64) |
| 摄像头 | USB 摄像头 或 CSI 摄像头, 分辨率 320×240（检测用） |
| 外设 | STM32 通过 UART 连接（GPIO14/15, TX/RX） |

---

## 二、AI 模型

### 2.1 模型清单

| 步骤 | 模型 | 文件 | 输入 | 输出 |
|------|------|------|------|------|
| 人脸检测 | SCRFD (det_500m) | `det_500m.param` / `.bin` | [1, 3, H, W] 动态尺寸 | 3 个 stride 共 16800 个 anchor: cls + bbox(4) + kps(10=5点×2) |
| 特征提取 | MobileFaceNet (w600k_mbf) | `w600k_mbf.param` / `.bin` | [1, 3, 112, 112] | [1, 512] 特征向量 |

### 2.2 检测模型输出结构（3 个 stride）

| Stride | anchors 数 | cls 输出 | bbox 输出 | kps 输出 |
|--------|-----------|----------|-----------|----------|
| 8 | 12800 (80×80×2) | [12800, 1] | [12800, 4] | [12800, 10] |
| 16 | 3200 (40×40×2) | [3200, 1] | [3200, 4] | [3200, 10] |
| 32 | 800 (20×20×2) | [800, 1] | [800, 4] | [800, 10] |

> 注：det_500m 是 InsightFace buffalo_sc 包中的 SCRFD 模型。若在 Pi 上推理延迟过高（>200ms/帧），可换用 buffalo_s 包中的 `det_160m`（anchor 数减半，精度略降但快约 3 倍）。

### 2.3 对齐基准点（112×112 标准脸）

```
左眼: (38.29, 51.70)    右眼: (73.53, 51.70)
鼻子: (56.02, 71.53)
左嘴角: (41.55, 92.37)   右嘴角: (70.55, 92.37)
```

用 SCRFD 输出的 5 个关键点 → `cv::getAffineTransform` / `estimateAffine2D` → `cv::warpAffine` 得到 112×112 对齐人脸。

---

## 三、软件环境搭建

### 3.1 系统基础

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential cmake git vim
```

### 3.2 OpenCV（apt 安装，不编译源码）

```bash
sudo apt install -y libopencv-dev
# 版本通常为 4.5.x~4.6.x，包含 opencv2/imgproc 等模块，够用
```

### 3.3 ncnn（源码编译，树莓派本地）

```bash
cd ~
git clone https://github.com/Tencent/ncnn.git
cd ncnn
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DNCNN_BUILD_EXAMPLES=OFF ..
make -j2   # 2GB 内存用 -j2，避免 OOM
sudo make install
sudo ldconfig
```

### 3.4 依赖总结

| 库 | 用途 | 安装方式 |
|----|------|----------|
| OpenCV | 摄像头采集、图像处理、仿射变换 | `apt install libopencv-dev` |
| ncnn | 神经网络推理 | 源码编译 |
| WiringPi / pigpio | GPIO 和 UART 控制 | `apt install wiringpi` 或 `apt install pigpio` |
| nlohmann/json | config.json 解析 | header-only，直接放入 `include/` |

---

## 四、软件模块设计

### 模块总览

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ 摄像头采集 │ →  │ 帧队列    │ →  │ AI 推理   │ →  │ 结果队列  │
│ (线程1)    │    │(线程安全) │    │ (线程2)   │    │(线程安全) │
└──────────┘    └──────────┘    └──────────┘    └────┬─────┘
                                                     │
                                          ┌──────────┴──────────┐
                                          │                     │
                                     ┌────┴────┐          ┌────┴────┐
                                     │ UART 发送│          │ 本地特征库│
                                     │ (线程3)  │          │ (内存)   │
                                     └────┬────┘          └─────────┘
                                          │        STM32
                                     ┌────┴────┐
                                     │ UART 接收│
                                     │ (线程4)  │
                                     └─────────┘
```

### 4.1 模块 1：摄像头采集与帧队列

```cpp
// 帧结构
struct FrameData {
    cv::Mat image;       // 采集的原始帧
    uint64_t timestamp;  // 毫秒时间戳
};

// 线程安全队列，容量 1（只保留最新帧）
class FrameQueue {
    std::mutex mtx;
    std::condition_variable cv;
    FrameData frame;
    bool has_new = false;
public:
    void push(FrameData f);   // 覆盖旧帧
    bool pop(FrameData &f);   // 取出最新帧，阻塞等待
};
```

**关键逻辑：**
- 采集线程以摄像头帧率（如 30fps）循环读取
- `push()` 直接覆盖旧帧，队列只保留最新一帧
- 推理线程 `pop()` 取最新帧，自动丢弃推理期间积累的中间帧
- 摄像头分辨率设为 320×240，减少后续 resize 开销

### 4.2 模块 2：AI 推理管线

```
detect()                     align()                  extract()
  帧 → 预处理 → SCRFD推理      bbox+kps → 仿射变换       112×112 → MobileFaceNet推理
       → NMS后处理             → 112×112 对齐脸           → 512维特征向量
```

```cpp
// 检测结果
struct DetectResult {
    float bbox[4];     // x1, y1, x2, y2
    float kps[10];     // 5个关键点 (x,y)
    float confidence;  // 置信度
};

// 推理上下文（程序启动时加载一次）
struct InferenceContext {
    ncnn::Net scrfd;   // SCRFD 检测网络
    ncnn::Net mbf;     // MobileFaceNet 特征网络
    float det_thresh;  // 检测阈值，默认 0.5
};

// 主推理函数
vector<FaceResult> run_inference(
    InferenceContext &ctx,
    const cv::Mat &frame,
    const FeatureDB &db);
```

**SCRFD 后处理流程：**
1. 3 个 stride 的 cls 输出做 sigmoid → 筛选 score > threshold 的 anchor
2. 对应位置的 bbox 做 decode（根据 anchor 中心和步长还原绝对坐标）
3. 对应位置的 kps 做 decode（还原 5 个关键点的绝对坐标）
4. NMS（IoU 阈值 0.4）去重，得到最终人脸列表
5. 所有人脸 bbox 外扩 10%（为对齐留余量）

**对齐流程：**
1. 用 SCRFD 输出的 kps[10]（5 个点）作为源点
2. 用基准 5 点（见 2.3 节）作为目标点
3. `cv::estimateAffine2D` 计算变换矩阵（5 点 → 相似变换或仿射变换）
4. `cv::warpAffine` 变换到 112×112

**比对流程：**
1. MobileFaceNet 前向推理，得到 512 维 float 向量
2. 与本地特征库中所有向量计算余弦相似度
3. 最高分 > 阈值（如 0.6）即判定为匹配身份
4. 无人匹配则标记为 "unknown"

### 4.3 模块 3：本地特征库

```cpp
class FeatureDB {
    struct Record {
        string id;              // 人员编号，如 "001"
        string name;            // 姓名
        vector<float> feature;  // 512 维特征向量
    };
    vector<Record> records;

public:
    bool load(const string &path);   // 从二进制文件加载
    bool save(const string &path);   // 保存到二进制文件
    void add(Record r);              // 添加人员
    bool remove(const string &id);   // 删除人员

    // 返回 (最佳匹配 ID, 相似度)，遍历所有记录算余弦相似度
    pair<string, float> match(const vector<float> &feat, float threshold);
};
```

**文件格式（简单二进制）：**
```
[4 字节] 记录数 N
每条记录:
  [4 字节] ID 长度  [变长] ID 字符串
  [4 字节] 姓名长度  [变长] 姓名 UTF-8
  [512×4 字节] 特征向量 (float[512])
```

> 初期用内存遍历比对即可。人员数量 < 1000 时，512 维余弦相似度遍历耗时 < 1ms，无需索引。

### 4.4 模块 4：STM32 UART 通信

#### 4.4.1 硬件连接

```
树莓派 4B              STM32
─────────              ──────
GPIO14 (TXD)  ──────►  RX
GPIO15 (RXD)  ◄──────  TX
GND           ───────  GND

UART 配置:
  波特率: 115200
  数据位: 8
  停止位: 1
  校验:   无
  流控:   无
  Pi 端设备文件: /dev/serial0 或 /dev/ttyAMA0
```

#### 4.4.2 帧协议定义

```
┌──────┬──────┬──────┬──────────┬──────┬──────┬──────┐
│ 帧头  │ 命令  │ 方向  │ 数据长度  │ 数据  │ 校验  │ 帧尾  │
│ 1B   │ 1B   │ 1B   │ 2B(H-L)  │ 变长  │ 1B   │ 1B   │
│ 0xAA │ CMD  │ DIR  │ LEN      │ DATA │ XOR  │ 0x55 │
└──────┴──────┴──────┴──────────┴──────┴──────┴──────┘

方向(DIR): 0x01=Pi→STM32, 0x02=STM32→Pi

校验: 从 CMD 开始到 DATA 最后一个字节，逐字节异或
转义: 数据区中若出现 0xAA 或 0x55，替换为 0xBB + (原字节^0xFF)
      0xBB 本身替换为 0xBB + 0x44
```

#### 4.4.3 命令字定义

```
Pi → STM32（识别结果下发）:
  CMD_IDENTIFY   = 0x10  数据: "ID,姓名,置信度"  例: "001,张三,0.92"
  CMD_UNKNOWN    = 0x11  数据: ""（检测到但未识别）
  CMD_NOFACE     = 0x12  数据: ""（画面中无人脸）
  CMD_HEARTBEAT  = 0x1F  数据: ""（心跳，每 5 秒）

STM32 → Pi（指令上传）:
  CMD_OPENDOOR   = 0x20  数据: ""（开门指令，可作识别触发信号）
  CMD_REGISTER   = 0x21  数据: "ID,姓名"（注册新用户指令）
  CMD_DELETE     = 0x22  数据: "ID"（删除用户）
  CMD_QUERY      = 0x23  数据: ""（查询当前库人数和状态）
```

#### 4.4.4 串口线程实现

```cpp
// UART 发送线程
void uart_send_thread(int fd, ResultQueue &queue, atomic<bool> &running) {
    while (running) {
        FaceResult result;
        if (queue.pop(result, 500ms)) {  // 500ms 超时
            vector<uint8_t> frame = encode_frame(result);
            write(fd, frame.data(), frame.size());
        }
        // 每 5 秒发一次心跳
        if (elapsed > 5s) send_heartbeat(fd);
    }
}

// UART 接收线程（非阻塞 + 缓冲区拼帧）
void uart_recv_thread(int fd, atomic<bool> &running) {
    vector<uint8_t> buf;
    while (running) {
        uint8_t byte;
        if (read(fd, &byte, 1) > 0) {
            buf.push_back(byte);
            // 尝试从 buf 中解析完整帧
            Frame f;
            if (decode_frame(buf, f)) {
                handle_command(f);  // 分发到对应处理函数
            }
        }
    }
}
```

---

## 五、主程序与线程调度

```cpp
// 全局上下文
struct AppContext {
    InferenceContext ai;
    FeatureDB db;
    FrameQueue frame_q;
    ResultQueue result_q;
    Config cfg;
    atomic<bool> running{true};
};

int main() {
    AppContext ctx;
    ctx.cfg.load("config.json");
    ctx.ai.load_models(ctx.cfg);
    ctx.db.load(ctx.cfg.feature_db_path);

    int uart_fd = open_uart(ctx.cfg.uart_device, 115200);

    // 启动线程
    thread t_capture(capture_thread, ref(ctx));
    thread t_inference(inference_thread, ref(ctx));
    thread t_uart_send(uart_send_thread, uart_fd, ref(ctx));
    thread t_uart_recv(uart_recv_thread, uart_fd, ref(ctx));

    // 主线程等待信号
    signal_handler(ctx);  // SIGINT/SIGTERM → ctx.running = false

    // 安全退出
    t_capture.join();
    t_inference.join();
    t_uart_send.join();
    t_uart_recv.join();
    close(uart_fd);
    return 0;
}
```

**线程规划（4 个线程，对应 4 核）：**

| 线程 | 职责 | 优先级 |
|------|------|--------|
| capture | 摄像头循环采集，push 帧队列 | 普通 |
| inference | 取帧 → SCRFD 检测 → 对齐 → MobileFaceNet 提取 → 比对 → push 结果队列 | 高 |
| uart_send | 从结果队列取结果，编码成帧，写串口 | 普通 |
| uart_recv | 读串口字节，拼帧，解析 STM32 指令 | 普通 |

---

## 六、配置文件

```json
{
    "camera": {
        "device": 0,
        "width": 320,
        "height": 240
    },
    "model": {
        "scrfd_param": "Model/ncnn_models/det_500m.param",
        "scrfd_bin": "Model/ncnn_models/det_500m.bin",
        "mbf_param": "Model/ncnn_models/w600k_mbf.param",
        "mbf_bin": "Model/ncnn_models/w600k_mbf.bin",
        "det_threshold": 0.5,
        "rec_threshold": 0.6,
        "nms_threshold": 0.4
    },
    "uart": {
        "device": "/dev/serial0",
        "baudrate": 115200
    },
    "feature_db": "data/features.bin"
}
```

---

## 七、项目目录结构

```
~/face_recognition/              # 树莓派上的工作目录
├── CMakeLists.txt
├── config.json
├── Model/
│   └── ncnn_models/
│       ├── det_500m.param
│       ├── det_500m.bin
│       ├── w600k_mbf.param
│       └── w600k_mbf.bin
├── src/
│   ├── main.cpp                 # 入口 + 调度
│   ├── camera.cpp               # 摄像头采集
│   ├── inference.cpp            # SCRFD + MobileFaceNet 推理
│   ├── scrfd.cpp                # SCRFD 后处理（decode + NMS）
│   ├── align.cpp                # 仿射变换对齐
│   ├── feature_db.cpp           # 本地特征库
│   ├── uart.cpp                 # 串口收发 + 帧编解码
│   └── config.cpp               # JSON 配置解析
├── include/
│   ├── camera.h
│   ├── inference.h
│   ├── feature_db.h
│   ├── uart.h
│   ├── config.h
│   └── types.h                  # FrameData, DetectResult, FaceResult 等结构体
├── data/
│   └── features.bin             # 持久化特征库
└── scripts/
    └── face_recog.service       # systemd 服务文件
```

---

## 八、CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(face_recognition)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native -mfpu=neon")

find_package(OpenCV REQUIRED)
find_package(ncnn REQUIRED)

add_executable(face_recog
    src/main.cpp
    src/camera.cpp
    src/inference.cpp
    src/scrfd.cpp
    src/align.cpp
    src/feature_db.cpp
    src/uart.cpp
    src/config.cpp
)

target_include_directories(face_recog PRIVATE include)
target_link_libraries(face_recog ncnn ${OpenCV_LIBS} pthread)
```

---

## 九、systemd 自启服务

```ini
# /etc/systemd/system/face_recog.service
[Unit]
Description=Face Recognition Service
After=multi-user.target

[Service]
Type=simple
ExecStart=/home/pi/face_recognition/build/face_recog
WorkingDirectory=/home/pi/face_recognition
Restart=always
RestartSec=5
User=pi
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable face_recog
sudo systemctl start face_recog
sudo systemctl status face_recog
journalctl -u face_recog -f   # 查看日志
```

---

## 十、开发顺序

```
Phase 1 — 验证 AI 管线（确认 Pi 能跑）
  □ 1.1 树莓派装系统、联网、SSH
  □ 1.2 安装 OpenCV(apt) + 编译 ncnn
  □ 1.3 把模型传到 Pi 上
  □ 1.4 写最小 demo：读一张图片 → SCRFD 检测 → 打印 bbox/kps
  □ 1.5 加入对齐 + MobileFaceNet 提取 → 打印 512 维向量
  □ 1.6 benchmark：记下检测/提取各耗时多少 ms

Phase 2 — 摄像头 + 特征库
  □ 2.1 写摄像头采集模块
  □ 2.2 采集 → 推理 串联，终端打印识别结果
  □ 2.3 实现 FeatureDB（加载、保存、添加、比对）

Phase 3 — STM32 通信
  □ 3.1 写 UART 协议编解码
  □ 3.2 写发送/接收线程
  □ 3.3 联调 STM32：识别结果下发 + 接收指令

Phase 4 — 工程化
  □ 4.1 config.json 解析
  □ 4.2 systemd 自启
  □ 4.3 长时间运行稳定性测试
```
