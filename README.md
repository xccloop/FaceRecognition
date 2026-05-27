# FaceRecognition — 四端协同人脸识别门禁系统

> 微信小程序 → Windows 后台 → 树莓派推理 → STM32 门锁 · 全链路闭环
> 作者：向治昌

---

## 架构图

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                         FaceRecognition 四端协同架构                            │
└──────────────────────────────────────────────────────────────────────────────┘

  Phone (微信小程序)       Windows (FastAPI+Vue)        Pi 4B (ncnn推理)         STM32 (FreeRTOS)
  ┌────────────────┐     ┌──────────────────┐     ┌──────────────────┐     ┌──────────────┐
  │  wx.chooseMedia │     │  /api/register    │     │  Flask :5000     │     │  UART DMA RX │
  │  wx.uploadFile  │────→│  SQLite + ORM    │────→│  C++ facerec     │     │  RingBuffer  │
  │                 │     │  SyncTask Queue  │     │  ONNX (备选)      │     │  COBS+CRC8   │
  │  扫码配置服务器  │     │  Worker 后台同步  │     │  features/*.bin  │     │  Frame Parser│
  │  最近使用列表    │     │  Vue3 管理面板    │     │                  │     │  cmd_handler │
  └────────────────┘     └──────────────────┘     └──────────────────┘     └──────────────┘
                                  │                        │                       │
                                  │  MJPEG :8080           │  UART 115200 8N1      │
                                  └────────────────────────┼───────────────────────┘
                                                           │
                                          ┌────────────────┴────────────────┐
                                          │  COBS(payload) | CRC8 | 0x00    │
                                          │  CMD: IDENTIFY/UNKNOWN/NOFACE   │
                                          │       MULTIFACE/HEARTBEAT/ACK   │
                                          └─────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════
                              通信链路一览
═══════════════════════════════════════════════════════════════════════════════

  链路              协议                    数据                    可靠性保障
 ─────────────────────────────────────────────────────────────────────────────
  手机 → Windows    HTTP POST multipart     照片 JPEG + 姓名         5MB 上限校验
  Windows → Pi      HTTP POST multipart     照片 + 姓名 (注册)       持久化重试队列
  Pi → STM32        UART COBS+CRC8          识别结果二进制帧          CRC8 检错/DMA ORE 恢复
  STM32 → Pi        UART COBS+CRC8          ACK / 状态反馈           IWDG 看门狗 4s
  Pi → Windows      HTTP MJPEG              MJPEG 视频流 (可选)      按需编码
```

---

## 各端任务与技术栈

### Phone — 人脸采集入口

| | |
|---|---|
| **任务** | 拍照/选图 → 输入姓名 → 上传 Windows 后台注册 |
| **语言/运行时** | JavaScript ES6, 微信小程序原生框架 (基础库 ≥3.3.4) |
| **核心 API** | `wx.chooseMedia`, `wx.uploadFile`, `wx.scanCode`, `wx.requirePrivacyAuthorize` |
| **辅助功能** | QR 码扫码配置服务器地址, 最近 3 条历史记录, 连接测试 |
| **关键文件** | `Phone/miniprogram/pages/register/register.js` |

### Windows — 数据中枢 + 管理面板

| | |
|---|---|
| **任务** | 接收手机注册, 管理用户数据, 异步同步到树莓派, 提供桌面管理界面 |
| **后端** | Python 3.12, FastAPI + Uvicorn, SQLAlchemy ORM + SQLite |
| **前端** | Vue 3 + TypeScript + Vite + Element Plus |
| **桌面壳** | pywebview (Edge WebView2), PyInstaller 单文件打包 |
| **关键文件** | `Windows/main.py` (API), `Windows/sync.py` (同步), `Windows/models.py` (ORM) |

**数据库表：** `users` (注册人员), `sync_tasks` (持久化同步队列), `system_status` (运行时状态单行表)

**API 端点：** `/api/register` (注册), `/api/dashboard` (仪表盘), `/api/users` (CRUD), `/api/users/{id}/sync` (手动同步), `/api/users/sync-all` (批量同步), `/api/config` (运行时配置), `/api/pi/status` (Pi 在线检测)

### Raspberry Pi 4B — 推理引擎 + 识别运行时

| | |
|---|---|
| **任务** | ① 接收注册请求, 提取+存储人脸特征 ② 摄像头实时检测→识别→UART 控制门锁 |
| **推理框架** | **ncnn** (腾讯开源, ARM NEON 优化, 无 GPU 依赖) |
| **AI 模型** | SCRFD `det_500m` (人脸检测) + MobileFaceNet `w600k_mbf` (512维特征提取) |
| **注册 API** | Flask :5000, C++ facerec 子进程 (或 ONNX 引擎备选) |
| **运行时** | Python 多线程: Capture → Inference → UART Control → UART Monitor |
| **视频流** | MJPEG Flask :8080, `multipart/x-mixed-replace` |

**C++ 推理管道 (`Linux/Model/src/`)：**
- `facedetector.cpp` — SCRFD 检测器, max-side 缩放, pad 32 对齐, 向量化解码, NMS
- `facealigner.cpp` — ArcFace 5 点仿射对齐 → 112×112 正脸
- `featureextractor.cpp` — MobileFaceNet 512 维特征 + L2 归一化
- `main.cpp` — CLI (`register/identify/compare/live`) + FeatureDB 内存特征库

**Python 运行时 (`Linux/Raspberry Pi/src/`)：**
- `inference.py` — 推理线程: 跳帧优化, IOU 跟踪, 连续 N 帧确认去抖
- `camera.py` — FrameQueue (容量 1, pop 阻塞), ResultQueue (FIFO, 满时丢旧)
- `feature_db.py` — 特征 CRUD + inotify 热加载 (运行时增删人员自动生效)
- `uart.py` — COBS+CRC8 帧协议 + 非阻塞串口收发线程
- `recognition.py` — ONNX Runtime 推理引擎 (C++ 未编译时的备选, .bin 完全兼容)

### STM32F103 — 门锁终端

| | |
|---|---|
| **任务** | UART DMA 接收 Pi 识别结果, TFTLCD 中文显示, LED 状态, 继电器控制 |
| **MCU** | STM32F103ZET6 (Cortex-M3, 64KB SRAM, 512KB Flash) |
| **RTOS** | FreeRTOS v10+, 静态内存分配 |
| **工具链** | Keil MDK (ARMCC v5), SPL 标准外设库 |
| **硬件** | 3.5寸 TFTLCD (NT35310, FSMC 并口), W25Q128 SPI Flash (GB2312 全字库), LED×2 |

**固件模块：**
- `Comms/` — UART DMA 驱动 (TX 链式 DMA + RX 环形 DMA + IDLE 中断), SPSC 无锁 RingBuffer
- `Protocol/` — COBS+CRC8 帧编解码 + 逐字节状态机解析器 + 命令分发 (IDENTIFY/UNKNOWN/NOFACE/MULTIFACE/HEARTBEAT/ACK)
- `BSP/` — LED, TFTLCD (含 GBK 全字库汉字渲染), SPI NOR Flash
- `User/main.c` — FreeRTOS 入口: UART RX 任务, 1s 心跳定时器, 5ms 帧超时定时器, IWDG 看门狗

---

## 与 Demo 项目的区别：为持续运行做的工程加固

一个毕设 demo 只需要"跑通一次"，一个能用的产品需要"跑 7×24 不挂"。以下是在裸 demo 基础上额外做的事：

### 通信链路可靠性

| 问题 | Demo 做法 | 本项目做法 |
|------|----------|-----------|
| **UART 电磁干扰** | 不管, 挂了重启 | ORE/NE/FE 中断处理 → 自动清除错误标志 → 重启 DMA RX → 清空脏数据缓冲区 → 错误计数器可观测 |
| **数据损坏检测** | 无校验 / 简单 XOR | CRC-8 (多项式 0x07, 256 字节查表), 帧级校验, 损坏帧静默丢弃并记录 |
| **串口粘包/半包** | 依赖超时猜测帧边界 | COBS 编码 → 天然的 0x00 帧分隔符 + 逐字节状态机解析 + 5ms 字节间超时保护 |
| **串口断连检测** | 无 | 5s 心跳帧 + 60s 超时判定 → LCD 显示"连接超时" + LED 闪烁告警 |
| **固件死锁** | 手动复位 | IWDG 独立看门狗 (4s 溢出), UART 任务每循环喂狗 |
| **Windows→Pi 同步失败** | 失败就失败了 | SyncTask 持久化队列 (SQLite) + 后台 Worker 轮询 + 指数退避重试 (2s/5s/10s, 最多 3 次) + 进程重启不丢任务 |

### 并发安全

| 问题 | Demo 做法 | 本项目做法 |
|------|----------|-----------|
| **C++ 特征库并发读写** | 不管, 崩了重启 | `std::mutex` 保护所有 public 方法, `save()` / `load()` / `identify()` / `compareAll()` 均加锁 |
| **Python 特征库热加载** | 重启进程 | `threading.RLock` + inotify 文件监控 (回退 2s 轮询), 运行时增删人员自动生效 |
| **Pi 多线程管线** | 全局变量乱传 | FrameQueue (容量 1, Condition 同步), ResultQueue (FIFO, Lock 保护), 明确的所有权传递 |

### 协议一致性

| 问题 | Demo 做法 | 本项目做法 |
|------|----------|-----------|
| **C 和 Python 实现不一致** | 各写各的, 跑通了就行 | 45 个 pytest 测试覆盖 COBS 编解码、CRC8 查表、帧 roundtrip、CRC 检错、解析器边界条件 |
| **测试自动化** | 手工串口助手测 | `tests/protocol/` — `test_cobs.py` (18 个), `test_crc8.py` (8 个), `test_frame.py` (19 个) |

### 运维可观测性

| 问题 | Demo 做法 | 本项目做法 |
|------|----------|-----------|
| **UART 错误统计** | 无 | `uart_dma_error_count()` / `uart_dma_error_restarts()` API, 可接入诊断输出 |
| **API 请求日志** | 无 | FastAPI 中间件记录每个请求的方法、路径、状态码、耗时到 `request.log` |
| **数据库状态** | 无 | `system_status` 表记录 Pi 在线状态、摄像头帧率、最后心跳时间 |
| **Pi 健康检查** | 无 | `/api/health` 返回已注册用户列表, Windows 端实时检测可达性 |

### 资源管理

| 问题 | Demo 做法 | 本项目做法 |
|------|----------|-----------|
| **DMA 缓冲区溢出** | 静默丢数据 | RingBuffer 设计容量 ≥ 2× UART 波特率帧间隔, 超限时 tail 推进避免死锁 |
| **看门狗** | 没有 | IWDG 4s 溢出, UART 任务每循环喂狗 — 任务卡死 = 系统复位 |
| **FreeRTOS 栈溢出** | 不管 | `vApplicationStackOverflowHook` 捕获, 静态内存分配 (无堆碎片化) |

---

## 快速开始

### Windows 后台

```bash
# 开发模式
cd Windows
pip install -r requirements.txt
python main.py                    # 服务 :8000, 管理面板自动弹出

# 打包
build_exe.bat                     # → dist/FaceRecognition.exe (~30MB)
```

### 手机小程序

微信开发者工具打开 `Phone/` → 设置服务器地址 (或扫码 `tools/qr-generator.html` 生成的 QR 码) → 拍照 → 注册。

### 树莓派

```bash
# 编译 C++ 推理引擎
cd Linux/Model && mkdir build && cd build
cmake .. -DNCNN_DIR=/path/to/ncnn -DOpenCV_DIR=/path/to/opencv
make -j4                          # → build/facerec

# 启动注册 API
cd Linux/Model
python pi_server.py --port 5000

# 启动识别运行时
cd Linux/Raspberry\ Pi
python main.py                    # 完整模式 (摄像头 + 推理 + UART + MJPEG)
python main.py --no-uart          # 调试模式 (仅推理 + 视频)
```

### STM32 固件

Keil MDK 打开 `Stm32/Project.uvprojx` → 编译 → 烧录。

### 硬件接线

```
树莓派 GPIO14 (TXD)  →  STM32 PA10 (USART1 RX)
树莓派 GPIO15 (RXD)  →  STM32 PA9  (USART1 TX)
树莓派 GND            →  STM32 GND
```

### 运行测试

```bash
cd FaceRecognition
python -m pytest tests/protocol/ -v    # 45 个协议一致性测试
```

---

## 项目结构

```
FaceRecognition/
├── Phone/                          # 微信小程序
│   └── miniprogram/pages/register/ # 拍照/上传/扫码配置
├── Windows/                        # Windows 后台
│   ├── main.py                     # FastAPI 入口 + pywebview 桌面
│   ├── models.py                   # SQLAlchemy ORM
│   ├── sync.py                     # Pi 同步模块 (持久化队列 + 重试)
│   ├── config.py                   # 运行时配置
│   ├── frontend/src/               # Vue 3 管理面板
│   ├── photos/                     # 用户照片存储
│   └── dist/                       # PyInstaller 打包输出
├── Linux/                          # 树莓派
│   ├── Model/                      # C++ ncnn 推理核心
│   │   ├── src/                    # facedetector, facealigner, featureextractor, main
│   │   ├── models/ncnn_models/     # SCRFD + MobileFaceNet (ncnn 格式)
│   │   ├── pi_server.py            # Flask 注册 API (:5000)
│   │   └── features/               # 人脸特征 .bin 存储
│   └── Raspberry Pi/src/           # Python 运行时
│       ├── camera.py               # 采集 + FrameQueue/ResultQueue
│       ├── recognition.py          # ONNX 推理引擎 (备选)
│       ├── inference.py            # 推理线程 (跳帧/IOU 跟踪/确认)
│       ├── feature_db.py           # 特征库 CRUD + 热加载
│       ├── uart.py                 # COBS+CRC8 帧协议 + 串口
│       └── mjpeg.py                # MJPEG 视频流 (:8080)
├── Stm32/                          # STM32 固件
│   ├── User/main.c                 # FreeRTOS 入口
│   ├── Comms/                      # DMA UART + RingBuffer
│   ├── Protocol/                   # COBS+CRC8 帧协议 + 命令处理
│   └── BSP/                        # LED/LCD/SPI/NOR Flash
├── tests/
│   └── protocol/                   # 协议一致性 pytest 测试 (45 个)
├── tools/                          # 辅助工具 (串口测试/QR 生成器)
└── docs/                           # 项目文档
```

---

## 当前状态

| 模块 | 状态 |
|------|------|
| 手机小程序 | 完成 |
| Windows 后台 | 完成 (v1.2.0, PyInstaller EXE) |
| Pi 注册 API | 完成 (Flask :5000) |
| Pi 实时识别 | 代码就绪 (待摄像头+STM32 硬件联调) |
| C++ ncnn 引擎 | 代码完成 (待 Pi cmake 编译) |
| STM32 固件 | 完成 (FreeRTOS + DMA UART + COBS/CRC8 + TFTLCD + SPI Flash 字库) |
| 协议测试 | 完成 (45 个 pytest, 全部通过) |
