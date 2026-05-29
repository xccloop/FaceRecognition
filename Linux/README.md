# Linux — 树莓派识别运行时

> 部署位置：`~/Desktop/FaceRec/`

## 技术栈

| 项目 | 说明 |
|------|------|
| 推理框架 | ncnn (腾讯开源, ARM NEON 优化, 2 线程) |
| AI 模型 | SCRFD `det_500m_dyn` (人脸检测) + MobileFaceNet `w600k_mbf_opt` (512维特征) |
| 语言 | C++17 (GCC 12.2, ARM aarch64) |
| 相机 | OpenCV 4.6 (V4L2) |
| 串口 | POSIX termios (`/dev/serial0`, 115200 8N1) |
| 进程管理 | systemd (开机自启, 崩溃自动重启) |
| 摄像头 | 320x240 USB 摄像头 (推理), MJPEG 640x480 缩放回传 |
| 视频流 | 内建 HTTP MJPEG (raw socket + TCP_NODELAY, :8080) |
| 注册 API | Flask :5000 (Python, ONNX Runtime 备选引擎) |

## 功能

**单进程 C++ 二进制 (`facerec run`)** 完成全部实时任务：
1. 摄像头采集 → ncnn 人脸检测 + 特征提取
2. IOU 多目标跟踪 + 连续帧确认去抖 (confirm=3)
3. UART COBS/CRC8 帧收发 → STM32 通信
4. MJPEG 视频流 (:8080) → Windows 端查看
5. 特征库每 5s 自动热加载 (运行时增删人员)

**Python 辅助**：
- `server/pi_server.py` (:5000) — 接收 Windows 端注册照片，ONNX 提取特征 → `features/*.bin`
- `server/recognition.py` — ONNX Runtime 推理引擎（pi_server 依赖）
- `uart_simulator.py` — 交互式串口测试工具

## 目录结构

```
~/Desktop/FaceRec/
├── Model/                        # C++ ncnn 推理核心
│   ├── src/
│   │   ├── main.cpp              # 入口，生产模式 `facerec run`
│   │   ├── facedetector.cpp      # SCRFD 检测器
│   │   ├── facealigner.cpp       # 5点仿射对齐 → 112×112
│   │   ├── featureextractor.cpp  # MobileFaceNet 特征提取
│   │   └── uart_protocol.cpp     # COBS+CRC8 帧编解码
│   ├── inc/                      # 头文件
│   ├── server/                   # Python 注册 API
│   │   ├── pi_server.py          # Flask 注册 API (:5000)
│   │   └── recognition.py        # ONNX Runtime 推理引擎
│   ├── models/ncnn_models/       # ncnn .param/.bin 模型
│   ├── features/                 # 人脸特征 .bin 存储
│   ├── build/facerec             # 编译产物 (ARM aarch64 ELF)
│   ├── mjpeg_stream.py           # MJPEG 推流 (备用, 已内建到 C++)
│   └── CMakeLists.txt            # CMake 构建配置
└── User/
    └── start.sh                  # 一键启动脚本
```

## 开发

### 首次部署

```bash
# 1. 编译 ncnn
cd /home/qxc/ncnn_build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DNCNN_BUILD_TOOLS=OFF \
      -DNCNN_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/usr/local ..
make -j2 && sudo make install && sudo ldconfig

# 2. 编译 C++ 项目
cd ~/Desktop/FaceRec/Model/build
cmake -DNCNN_DIR=/usr/local ..
make -j2
```

### 启动

```bash
# 一键启动
cd ~/Desktop/FaceRec && ./User/start.sh

# 或 systemd (开机自启)
sudo systemctl start face-recognition
journalctl -u face-recognition -f
```

### 日志格式

```
[Run] #510 fps=30 detect=30ms cpu=43C 190% tracks=1 uart=IDENTIFY:向治昌
        │     │      │         │          │        │        │
        │     │      │         │          │        │        └─ UART命令
        │     │      │         │          │        └─ 跟踪人脸数
        │     │      │         │          └─ CPU温度+占用率
        │     │      │         └─ 检测耗时(30帧平均)
        │     │      └─ 实时帧率
        │     └─ 总帧数
        └─ 模块标识
```

## UART 接线

```
树莓派 Pin 8  (GPIO14 TXD) → STM32 PA10 (USART1 RX)
树莓派 Pin 10 (GPIO15 RXD) → STM32 PA9  (USART1 TX)
树莓派 Pin 6  (GND)        → STM32 GND
```

拔掉 STM32 板上 PA9/RXD 和 PA10/TXD 跳线帽。STM32 用 USB SLAVE 口独立供电。

## 诊断

```bash
vcgencmd measure_temp          # 温度
vcgencmd get_throttled         # 降频状态 (0x0=正常)
journalctl -u face-recognition -f  # 实时日志
ss -tlnp | grep -E "8080|5000"    # 端口检查
```

完整故障排查见 `Linux/doc/troubleshooting.md`。
