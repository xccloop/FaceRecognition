# 树莓派端 — 待完成事项

## 1. 推理管线修复（最高优先级）

**问题：** C++ ncnn 版加载 SCRFD 模型时，自定义层 Shape/Gather 的 pass-through 实现不准确，导致推理结果为空或崩溃。参见 `Model/doc/troubleshooting.md` 问题 6-7。

**方案优先级：**

- **方案 A（推荐先试）：** 用 pnnx 工具重新转换 ONNX → ncnn。pnnx 对动态形状处理比 onnx2ncnn 更好，可能直接消除 Shape/Gather 自定义层。

- **方案 B：** 完善自定义层实现。当前 Shape 层只输出 `[h,w]`，但 SCRFD 的 Shape 计算链涉及更多维度操作。需要 dump 出 ONNX 中间节点的实际值，逐一对照实现。

- **方案 C（保底）：** 放弃 ncnn C++，改用 Python + onnxruntime。verify.py 已验证功能完整可用，树莓派上 `pip install onnxruntime` 即可运行。性能会降到 2-5 FPS，但对于门禁类场景够用。

## 2. 线程安全队列

当前没有任何队列实现，需要在 `include/` 下新建 `types.h`，实现两个模板类：

- **FrameQueue** — 容量为 1 的帧队列。push 覆盖旧帧，pop 阻塞等待新帧。用于摄像头 → 推理线程之间。

- **ResultQueue** — 容量为 N（如 16）的结果队列。FIFO，push 非阻塞（满了就丢旧的），pop 阻塞等待。用于推理 → UART 发送线程之间。

关键：使用 `std::mutex` + `std::condition_variable`，`std::atomic<bool>` 支持安全退出。

## 3. 摄像头采集模块

新建 `src/camera.cpp` + `include/camera.h`：

- 封装 `cv::VideoCapture`，打开指定摄像头设备
- 设置分辨率（默认 320×240）
- 循环读取帧，构造 `FrameData{timestamp, cv::Mat}` push 到 FrameQueue
- 支持 `atomic<bool>& running` 安全退出
- 采集线程函数签名：`void captureLoop(FrameQueue& q, const Config& cfg, atomic<bool>& running)`

## 4. 特征库 FeatureDB

新建 `src/feature_db.cpp` + `include/feature_db.h`：

- 加载：从 `data/features.bin` 读取全部记录到内存 vector
- 保存：将内存 vector 写回 `data/features.bin`
- 添加：新增人员（ID + 姓名 + 512 维特征），自动保存
- 删除：按 ID 删除，自动保存
- 匹配：遍历所有记录，计算余弦相似度（特征已 L2 归一化，点积即相似度），返回最高分 > 阈值的匹配结果
- 支持热加载：监控 features/ 目录下 .bin 文件变化（可选，初期手动触发即可）

**二进制文件格式：**
```
[4 字节] 记录数 N (int32)
每条记录:
  [4 字节] ID 长度      [变长] ID 字符串 (UTF-8)
  [4 字节] 姓名长度      [变长] 姓名字符串 (UTF-8)
  [512×4 字节] 特征向量  (float[512], little-endian)
```

## 5. UART 通信模块

新建 `src/uart.cpp` + `include/uart.h`：

### 5.1 串口初始化
- 打开 `/dev/serial0`（或 config 中配置的设备路径）
- 配置：115200-8-N-1，无流控
- 封装为 `int openUart(const string& device, int baudrate)`

### 5.2 帧编码
- 输入：命令字 + 方向 + 数据字符串
- 输出：完整的帧字节数组
- 实现：帧头 0xAA → CMD → DIR → LEN(2B) → DATA → XOR 校验 → 帧尾 0x55
- 数据区转义：0xAA→0xBB+0x55, 0x55→0xBB+0xAA, 0xBB→0xBB+0x44

### 5.3 帧解码
- 输入：字节流缓冲区（引用）
- 输出：解析出的帧结构（CMD + 数据），或"不完整/无效"
- 状态机：搜帧头 0xAA → 读 CMD → 读 DIR → 读 LEN → 读 DATA + 反转义 → 读 XOR 校验 → 读帧尾 0x55
- 校验失败则丢弃整帧，重新搜帧头

### 5.4 发送线程
```cpp
void uartSendLoop(int fd, ResultQueue& q, atomic<bool>& running) {
    while (running) {
        FaceResult r;
        if (q.pop(r, 500ms)) {
            auto frame = encodeIdentifyResult(r);
            write(fd, frame.data(), frame.size());
        }
        // 每 5 秒心跳
    }
}
```

### 5.5 接收线程
```cpp
void uartRecvLoop(int fd, CommandQueue& cmd_q, atomic<bool>& running) {
    vector<uint8_t> buf;
    while (running) {
        uint8_t b;
        if (read(fd, &b, 1) > 0) {
            buf.push_back(b);
            Frame f;
            if (decodeFrame(buf, f)) {
                cmd_q.push(f);
            }
        }
    }
}
```

## 6. 推理线程

新建 `src/inference.cpp` + `include/inference.h`：

- 从 FrameQueue 取帧
- 调用 FaceDetector::detect() → 若无人脸，push UNKNOWN 或 NOFACE 到 ResultQueue
- 对每张人脸：FaceAligner::align() → FeatureExtractor::extract()
- 对每个特征：FeatureDB::match() → 得到最佳匹配结果
- 将 `FaceResult{id, name, confidence}` push 到 ResultQueue
- 推理耗时计时，超过 200ms 打印警告

## 7. 配置文件解析

新建 `src/config.cpp` + `include/config.h`：

- 引入 `nlohmann/json.hpp`（header-only，放入 include/）
- 加载 config.json，解析为 Config 结构体
- Config 包含：camera 参数、model 路径、UART 参数、特征库路径、识别阈值

```json
{
    "camera": { "device": 0, "width": 320, "height": 240 },
    "model": {
        "scrfd_param": "Model/det_500m.param",
        "scrfd_bin":   "Model/det_500m.bin",
        "mbf_param":   "Model/w600k_mbf.param",
        "mbf_bin":     "Model/w600k_mbf.bin",
        "det_threshold": 0.5,
        "rec_threshold": 0.4,
        "nms_threshold": 0.4
    },
    "uart": { "device": "/dev/serial0", "baudrate": 115200 },
    "feature_db": "data/features.bin"
}
```

## 8. 主程序 main.cpp 重构

将当前 CLI 工具（register/identify/compare/test）保留为 `src/cli_main.cpp` 或通过命令行参数切换。新增的实时识别模式实现：

```cpp
int main() {
    AppContext ctx;
    ctx.cfg = loadConfig("config.json");
    ctx.ai.loadModels(ctx.cfg);
    ctx.db.load(ctx.cfg.feature_db_path);

    int uart_fd = openUart(ctx.cfg.uart.device, ctx.cfg.uart.baudrate);

    thread t1(captureLoop, ref(ctx.frame_q), ref(ctx.cfg), ref(ctx.running));
    thread t2(inferenceLoop, ref(ctx));
    thread t3(uartSendLoop, uart_fd, ref(ctx.result_q), ref(ctx.running));
    thread t4(uartRecvLoop, uart_fd, ref(ctx.cmd_q), ref(ctx.running));

    // 信号处理：SIGINT/SIGTERM → ctx.running = false
    // 等待四个线程 join
    // 关闭串口、保存特征库
}
```

## 9. systemd 自启服务

新建 `scripts/face_recog.service`，内容见 `Linux scheme.md` 第九节。部署后：
```bash
sudo cp scripts/face_recog.service /etc/systemd/system/
sudo systemctl enable face_recog
sudo systemctl start face_recog
```

## 10. CMakeLists.txt 更新

当前 CMakeLists.txt（`Model/CMakeLists.txt`）只编译 CLI 工具。需要：

- 升级为项目级 CMakeLists.txt（放在 `Linux/` 下）
- 添加新增源文件：camera.cpp, inference.cpp, feature_db.cpp, uart.cpp, config.cpp
- 链接 pthread
- 条件编译：Windows 下跳过串口模块（无 /dev/tty）
