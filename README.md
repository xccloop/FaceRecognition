# FaceRecognition — 四端协同人脸识别门禁系统

> **架构**: 微信小程序采集人脸 → Windows 后台管理存储 → 树莓派 4B 运行 C++/ncnn 推理引擎 → STM32 控制门锁
> **作者**: 向治昌

---

## 一、系统架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                       四端协同人脸识别门禁系统                              │
└──────────────────────────────────────────────────────────────────────────┘

  手机小程序                   Windows 后台                   树莓派 4B                     STM32
 (微信小程序)               (FastAPI + Vue 3)           (Flask + C++/ncnn 引擎)        (FreeRTOS)

 ┌──────────┐    HTTP      ┌──────────────┐    HTTP     ┌───────────────┐    UART     ┌──────────┐
 │          │  multipart   │              │  /api/      │               │  COBS+     │          │
 │ 拍照上传  │ ───────────→ │  数据中枢     │ register   │  C++ facerec  │  CRC8      │ 门锁控制  │
 │          │  photo+name  │  :8081       │ ──────────→ │  ONNX(备选)    │ ─────────→ │ :115200  │
 │ QR扫码配置│ ←─────────── │  SQLite      │            │  :5000        │            │ 继电器    │
 │          │              │  Vue 3 管理   │            │  features/    │            │ LED/蜂鸣  │
 └──────────┘              └──────────────┘            │  .bin 特征    │            └──────────┘
                                │                      └───────────────┘
                                │  MJPEG 视频流                │
                                │  :8080/video                │
                                └─────────────────────────────┘

══════════════════════════════════════════════════════════════════════════════
                              通信协议总览
══════════════════════════════════════════════════════════════════════════════

 链路               协议                     数据                      端口/参数
 ─────────────────────────────────────────────────────────────────────────────
 手机 → Windows     HTTP POST multipart      照片 JPEG + 姓名           :8081
 手机 ← Windows     HTTP GET JSON            服务器地址、健康检查        :8081
 Windows → Pi       HTTP POST multipart      照片 + 姓名 (注册)         :5000
 Windows ← Pi       HTTP GET JSON            已注册用户列表、健康检查    :5000
 Windows ← Pi       HTTP MJPEG               实时视频流 (可选)           :8080
 Pi → STM32         UART COBS+CRC8           识别结果二进制帧           115200 8N1
 Pi ← STM32         UART COBS+CRC8           ACK / 状态反馈            115200 8N1
```

---

## 二、各模块详解

### 2.1 手机小程序 — 人脸采集入口

| 项目 | 内容 |
|------|------|
| **职责** | 采集用户人脸照片，上传到 Windows 后台注册 |
| **语言/框架** | JavaScript ES6, 微信小程序原生框架 (基础库 ≥3.3.4) |
| **关键 API** | `wx.chooseMedia`, `wx.uploadFile`, `wx.scanCode` |
| **关键文件** | `Phone/miniprogram/pages/register/register.js`, `.wxml`, `.wxss` |

**拍照上传流程：**

```
用户点击拍照 → wx.requirePrivacyAuthorize (隐私授权)
  → wx.chooseMedia({ sourceType: ["camera"], sizeType: ["compressed"] })
  → 微信内置压缩 (不二次压缩，保护识别率)
  → 输入姓名 (非空校验, ≤20 字)
  → wx.uploadFile → POST /api/register (multipart: photo + name)
  → 返回 { success, face_score, pi_sync_status }
```

**服务器配置：**

- **扫码配置**: `wx.scanCode()` 解析 QR 码中的 `http://ip:port`，自动填入
- **手动配置**: 输入 IP + 端口
- **连接测试**: `HEAD /api/health` 验证服务器可达
- **最近使用**: 本地存储 3 条历史，支持选择/单条删除
- **URL 规范化**: 自动去路径、去尾斜杠，统一为 `http://ip:port`

**通信：**
```
POST /api/register   (multipart/form-data: photo + name)  → 注册
HEAD /api/health     (连接测试)                            → 200/超时
```

---

### 2.2 Windows 后台 — 数据中枢 + 管理面板

| 项目 | 内容 |
|------|------|
| **职责** | 接收手机上传、管理用户数据、同步到 Pi、提供原生桌面管理界面 |
| **语言/框架** | Python 3.12, FastAPI + Uvicorn, SQLAlchemy + SQLite |
| **前端** | Vue 3 + TypeScript + Vite + Element Plus |
| **桌面壳** | pywebview (Edge WebView2), PyInstaller 单文件打包 (~30MB) |
| **关键文件** | `Windows/main.py` (API), `sync.py` (Pi 同步), `models.py` (ORM), `config.py` (配置) |

**数据库 (SQLite, `face_recognition.db`)：**

```sql
-- 注册用户表
users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,             -- 姓名
    photo_url TEXT,                 -- 照片路径 (photos/<uuid>.jpg)
    pi_synced BOOLEAN DEFAULT 0,    -- 是否已同步到 Pi
    pi_sync_status TEXT,            -- pending / synced / failed
    pi_sync_error TEXT,             -- 上次同步失败原因
    created_at DATETIME
)

-- 同步任务队列 (持久化, 进程重启不丢失)
sync_tasks (
    id INTEGER PRIMARY KEY,
    user_id INTEGER FK → users.id,
    status TEXT,                    -- pending / processing / completed / failed
    retry_count INTEGER DEFAULT 0,
    next_retry_at DATETIME,         -- 下次重试时间
    error_message TEXT,
    created_at DATETIME
)

-- 系统运行时状态 (单行表, id=1)
system_status (
    id INTEGER PRIMARY KEY DEFAULT 1,
    pi_online BOOLEAN,              -- Pi API 是否可达
    pi_uptime TEXT,                 -- Pi 运行时间
    camera_fps REAL,                -- 摄像头帧率
    last_heartbeat DATETIME,        -- 最后心跳时间
    updated_at DATETIME
)
```

**API 端点：**

| 方法 | 路径 | 说明 |
|------|------|------|
| `POST` | `/api/register` | 手机上传注册 (multipart: photo + name)，照片存 `photos/`，创建 User + SyncTask |
| `GET` | `/api/dashboard` | 仪表盘数据: 注册人数、Pi 在线、同步失败数、最近 5 条注册 |
| `GET` | `/api/users` | 人员列表 (分页 `?page=&page_size=` + 搜索 `?search=`) |
| `DELETE` | `/api/users/{id}` | 删除人员 (仅删 DB 记录, 保留照片文件) |
| `DELETE` | `/api/users/{id}/pi` | 仅从 Pi 删除特征 (.bin 文件), Windows 数据保留 |
| `GET` | `/api/users/{id}/sync-status` | 查询单个用户 Pi 同步状态 |
| `POST` | `/api/users/{id}/sync` | 手动同步单个用户到 Pi |
| `POST` | `/api/users/sync-all` | 批量同步所有未同步用户 |
| `GET` | `/api/pi/status` | Pi 连接状态 (HEAD /api/health) |
| `GET/PUT` | `/api/config` | 运行时配置读写 (Pi 地址、端口等) |

**Pi 同步机制：**

```
注册请求 → 创建 User + SyncTask(status=pending)
                │
后台 worker 每 30s 轮询 ──→ 取出 pending 任务
                │
       ┌───────┴───────┐
       │ GET /api/features  │  去重检查: Pi 已有同名用户?
       │ (Pi :5000)        │  降级开关 DEDUP_REQUIRED=False
       └───────┬───────┘
               │ 未注册
       ┌───────┴───────┐
       │ POST /api/register │  发送 photo + name 到 Pi
       │ (Pi :5000)        │  Pi 调用 C++ facerec (或 ONNX) 提取特征
       └───────┬───────┘
               │
       成功 → status=completed    失败 → retry_count++
              user.pi_synced=True          next_retry_at = now + [2,5,10]s
                                          最多 3 次重试, 耗尽 → status=failed
```

**桌面窗口：** PyInstaller 打包时嵌入 Vue 前端 dist + FastAPI 后端, pywebview 创建 1200×800 原生窗口, 服务在后台线程运行。双击 `FaceRecognition.exe` 即可, 无需安装 Python。

---

### 2.3 树莓派 — C++/ncnn 推理引擎 + 识别运行时

| 项目 | 内容 |
|------|------|
| **职责** | ①接收 Windows 注册请求, 提取+存储人脸特征 ②摄像头实时检测→识别→UART 控制门锁 |
| **推理框架** | **ncnn** (腾讯开源, ARM NEON 优化, 无 GPU 依赖) |
| **AI 模型** | SCRFD `det_500m` (人脸检测, 3 个 stride level) + MobileFaceNet `w600k_mbf` (特征提取, 512 维) |
| **注册 API** | Flask (Python) 监听 :5000, 调用 C++ `facerec` 子进程完成检测→对齐→提取→保存 |
| **运行时** | Python 多线程 (cv2 采集 → ncnn/ONNX 推理 → UART 控制) |
| **语言** | C++17 (ncnn 推理核心) + Python 3 (服务层/管线) |
| **关键文件** | `Linux/Model/src/` (C++), `Linux/Model/pi_server.py` (注册 API), `Linux/Raspberry Pi/` (运行时) |

#### 2.3.1 C++ ncnn 推理核心 (`Linux/Model/src/`)

**源码结构：**

```
Linux/Model/
├── inc/
│   ├── facedetector.h        # FaceDetector 类声明
│   ├── facealigner.h         # FaceAligner 静态类声明
│   └── featureextractor.h    # FeatureExtractor 类声明
├── src/
│   ├── facedetector.cpp      # SCRFD 检测器实现 (ncnn)
│   ├── facealigner.cpp       # 5点仿射对齐实现 (OpenCV)
│   ├── featureextractor.cpp  # MobileFaceNet 特征提取实现 (ncnn)
│   └── main.cpp              # CLI 入口 + FeatureDB + Live 模式
├── models/
│   ├── ncnn_models/          # ncnn 格式模型 (.param + .bin)
│   │   ├── det_500m_dyn.*    # SCRFD 检测 (动态 Interp 层)
│   │   └── w600k_mbf.*       # MobileFaceNet 特征提取
│   └── onnx_models/          # ONNX 原始模型 (备份/备选)
│       └── buffalo_s/
│           ├── det_500m.onnx
│           └── w600k_mbf.onnx
├── features/                 # 人脸特征存储 (.bin 文件)
├── pi_server.py              # Flask 注册 API
└── CMakeLists.txt            # CMake 构建 (ncnn + OpenCV)
```

**FaceDetector — SCRFD 人脸检测 (`facedetector.cpp`)：**

```
输入: BGR 图像 (H,W,3) uint8
  │
  ├─ max-side 缩放 (默认 480px, 仅缩小不放大)
  ├─ pad 到 32 的倍数 (FPN 对齐 padding, 灰色 128)
  ├─ ncnn::Mat::from_pixels → subtract_mean_normalize ([127.5] / [128])
  │
  ├─ 推理: det_500m (3 个 stride level: 8, 16, 32)
  ├─ 每个 level 输出: scores (1×N), bboxes (4×N), keypoints (10×N)
  │
  ├─ 解码: generateAnchors() 预计算 anchor 中心坐标
  │   对每个 anchor:
  │     x1 = cx - bx*stride     y1 = cy - by*stride
  │     x2 = cx + bw*stride     y2 = cy + bh*stride
  │     keypoint[i] = anchor_center + kps_offset * stride
  │
  ├─ NMS: 按 score 降序 → 贪心 IoU 剔除 (阈值 0.4)
  ├─ 过滤: 框中心在 pad 区域外的丢弃
  └─ 输出: vector<FaceInfo> (x1,y1,x2,y2, score, keypoints[5][2])
```

**FaceAligner — 5 点仿射对齐 (`facealigner.cpp`)：**

```
输入: 原图 + 5 个关键点
  │
  ├─ 参考模板 (ArcFace 标准, 112×112):
  │   (30.29,51.69)  (65.53,51.69)  (48.02,71.73)
  │   (33.54,92.36)  (62.72,92.36)
  │
  ├─ cv::estimateAffinePartial2D(src_kps, ref_kps)
  ├─ cv::warpAffine → 112×112 标准正脸
  └─ 输出: 112×112 BGR 图像
```

**FeatureExtractor — MobileFaceNet 特征提取 (`featureextractor.cpp`)：**

```
输入: 112×112 BGR 对齐人脸
  │
  ├─ ncnn::Mat::from_pixels → subtract_mean_normalize ([127.5] / [127.5])
  ├─ 推理: w600k_mbf → 输出 "516" 层
  ├─ L2 归一化: feat[i] /= sqrt(Σ feat² + 1e-8)
  └─ 输出: vector<float> 512 维 (|feat| = 1.0)
```

**main.cpp CLI 命令：**

```bash
facerec register <photo.jpg> <name>    # 注册 (检测→对齐→提取→保存 .bin)
facerec identify <photo.jpg>           # 身份识别 (vs 已注册库)
facerec compare <a.jpg> <b.jpg>        # 1:1 人脸比对
facerec live                           # 摄像头实时识别 (按 q 退出, r 注册, i 识别)
```

**FeatureDB (C++ 内嵌类)：**
- 注册库加载: 读取 `features/*.bin` → 内存 `vector<Entry>`
- 保存: `features/<name>.bin` = 4B dim (int32) + N×4B float32
- 匹配: 余弦相似度 (点积, 特征已 L2 归一化), 阈值 0.4

#### 2.3.2 pi_server.py — 注册 API 服务 (`Linux/Model/pi_server.py`)

Flask HTTP 服务 (:5000), 供 Windows 后台调用：

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/health` | 在线检测, 返回 `{ status, registered_users, users[] }` |
| `POST` | `/api/register` | 接收 multipart photo + name, 调用推理引擎提取特征, 保存 .bin |
| `GET` | `/api/features` | 列出已注册用户 |
| `DELETE` | `/api/features?name=xxx` | 删除指定用户 .bin 特征文件 |

**注册流程：**
```
接收 photo + name
  → cv2.imdecode (bytes → BGR 数组)
  → 调用推理引擎 (C++ facerec 子进程 或 ONNX 引擎)
  → 检测 → 对齐 → 提取 → 保存 features/<name>.bin
  → 返回 { success, face_score, feature_dim }
```

> **引擎模式**: 优先调用 C++ `facerec` 二进制 (编译后路径 `build/facerec`), 若未编译则自动回退到 Python ONNX 引擎 (`Raspberry Pi/src/recognition.py`), 两者 .bin 格式完全兼容。

#### 2.3.3 实时识别运行时 (`Linux/Raspberry Pi/`)

Python 多线程架构, 负责摄像头采集→推理→UART 控制→MJPEG 视频流的完整管线。

**四线程管线：**

```
┌─────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ Capture     │    │ Inference    │    │ UART Control │    │ UART Monitor │
│ Thread      │    │ Thread       │    │ Thread       │    │ Thread       │
├─────────────┤    ├──────────────┤    ├──────────────┤    ├──────────────┤
│ 摄像头读取   │    │ FrameQueue   │    │ ResultQueue  │    │ 串口接收      │
│ → 640×480   │───→│ .pop() 取帧  │───→│ .pop() 取结果│    │ 监听 STM32   │
│ → JPEG 编码  │    │              │    │              │    │ 应答 (ACK)   │
│ → FrameQueue │    │ ① detect     │    │ COBS+CRC8    │    │              │
│   .push()    │    │ ② IOU track  │    │ 编码 → 发帧  │    │              │
│              │    │ ③ align      │    │              │    │              │
│              │    │ ④ extract    │    │ CMD_IDENTIFY │    │              │
│              │    │ ⑤ match      │    │ CMD_UNKNOWN  │    │              │
│              │    │ → ResultQueue│    │ CMD_HEARTBEAT│    │              │
└─────────────┘    └──────────────┘    └──────────────┘    └──────────────┘
       │                                        │
       └──────── MJPEG Server ──────────────────┘
            Flask :8080 /video (multipart/x-mixed-replace)
```

**推理线程核心逻辑 (`inference.py`)：**

```
while running:
    frame = frame_q.pop(timeout=1.0)

    if frame_count % skip_frames != 1:  # 跳帧优化 (默认每3帧推理一次)
        continue

    detections = engine.detect(frame, score_thresh=0.5, max_side=480)

    # IOU 跟踪: 贪心匹配新检测 ↔ 已有追踪
    matched, unmatched = _match_detections_to_tracks(detections, tracks)

    for each matched track:
        更新 bbox
        if 距上次提取 > 2秒:
            aligned = engine.align(frame, keypoints)      # 5点仿射
            feat = engine.extract(aligned)                 # 512维
            name, sim = feature_db.match(feat, threshold=0.4)

            if 连续 confirm_frames 帧识别为同一人:
                result_q.push({type: "identify", name, confidence})
                track.result_sent = True

    for each unmatched detection:
        创建新 TrackedFace, 分配 track_id

    清理超时 track (2秒未更新)
```

**特征库 (`feature_db.py`)：**
- 二进制格式: `[4B dim (int32 LE)] [N×4B float32 LE]` — 与 C++ FeatureDB 完全兼容
- 内存缓存全量特征 (dict `name → np.ndarray`)
- 热加载: inotify 监控 `features/` 目录 (回退 2s 轮询), 运行时新增/删除人员自动生效

**UART 串口 (`uart.py`)：**

```
帧格式 (v2 COBS+CRC8):
  ┌──────────────────────────────────┬──────┬──────┐
  │ COBS(CMD+DIR+LEN_H+LEN_L+DATA)   │ CRC8 │ 0x00 │
  └──────────────────────────────────┴──────┴──────┘

CRC8 参数: 多项式 0x07, 初始值 0x00, 无反射, 256 字节查表

编解码函数:
  crc8_compute(data) → uint8
  cobs_encode(src) → bytes
  cobs_decode(src) → bytes
  frame_encode(cmd, data) → bytes   # COBS(payload) + CRC8 + 0x00
  frame_decode(raw) → dict          # 返回 {cmd, dir, data}

FrameParser: 逐字节状态机 (WAIT → DATA), 匹配 STM32 frame_parser_feed 逻辑

控制线程: ResultQueue → 编码 → serial.write, 心跳每 5s
监听线程: serial.read → 逐字节喂 FrameParser → 解码 → on_message 回调
```

---

### 2.4 STM32 — 门锁控制终端

| 项目 | 内容 |
|------|------|
| **职责** | UART DMA 接收 Pi 识别结果, COBS+CRC8 帧解析, 控制继电器/LED/蜂鸣器 |
| **MCU** | STM32F103C8T6 (Cortex-M3, 64KB SRAM, 128KB Flash) |
| **RTOS** | FreeRTOS v10+ (静态内存分配, 无动态 malloc) |
| **工具链** | Keil MDK (ARMCC v5), SPL (Standard Peripheral Library) |
| **关键文件** | `Stm32/User/main.c`, `Stm32/Protocol/frame_protocol.c/.h`, `Stm32/Comms/` |

**固件架构 (FreeRTOS 任务 + 定时器)：**

```
┌─────────────────────────────────────────────────────────────┐
│                      FreeRTOS Kernel                         │
├──────────────┬──────────────┬──────────────┬────────────────┤
│ vUartRxTask  │ vRelayTask   │ vLedTask     │ 软件定时器      │
│ (优先级 3)    │ (优先级 2)   │ (优先级 1)   │                │
├──────────────┤              │              ├────────────────┤
│ DMA 循环模式  │ 继电器 GPIO   │ LED 闪烁      │ Heartbeat 检查  │
│ RingBuffer   │ 开门时序控制  │ 蜂鸣器控制    │ (1s 周期)       │
│ IDLE 中断    │              │              │                │
│ FrameParser  │              │              │ IWDG 喂狗       │
│ 命令分发      │              │              │ (500ms 周期)    │
└──────────────┴──────────────┴──────────────┴────────────────┘
```

**UART DMA 三层驱动栈：**

```
┌──────────────────────────────────────────────────┐
│  Layer 3: uart_api    │  互斥锁保护, 阻塞/异步    │
│                        │  发送, 接收, printf     │
├────────────────────────┼─────────────────────────┤
│  Layer 2: uart_dma     │  TX: 链式 DMA (分段)     │
│                        │  RX: 循环 DMA + IDLE 中断│
│                        │  ISR → vTaskNotifyGive   │
├────────────────────────┼─────────────────────────┤
│  Layer 1: uart_port    │  USART1 DMA1 寄存器操作   │
│                        │  CNDTR 追踪, 中断处理     │
├────────────────────────┼─────────────────────────┤
│  RingBuffer (SPSC)     │  无锁, 容量 2^n          │
│                        │  连续读写长度查询         │
└──────────────────────────────────────────────────┘
```

**COBS+CRC8 帧协议 (`frame_protocol.c`)：**

```
                    Pi → STM32 帧格式

  编码:
    payload = CMD(1B) | DIR(1B) | LEN_H(1B) | LEN_L(1B) | DATA(0~N字节)
    frame   = COBS(payload) | CRC8(payload) | 0x00

  解码 (frame_parser_feed 状态机):
    收字节直到 0x00 → 最后一个非零字节是 CRC8
    → COBS 解码前面部分还原 payload
    → 计算 CRC8(payload) 与收到的 CRC8 比较
    → 提取 cmd, dir, len, data

  参数:
    波特率          115200 8-N-1
    帧分隔符         0x00 (唯一)
    数据最大长度     64 字节
    字节超时         5ms (定时器检测, 超时复位状态机)
    CRC8 多项式      0x07 (CRC8-CCITT)
    CRC8 初始值      0x00, 无反射
    最大帧长         约 70 字节 (COBS 膨胀 + 校验 + 分隔符)
```

**命令字：**

| CMD | 值 | 方向 | 含义 | STM32 动作 |
|-----|-----|------|------|-----------|
| `CMD_IDENTIFY` | `0x10` | Pi → STM32 | 识别成功 (data=姓名 UTF-8) | 发送 ACK + 继电器开门 |
| `CMD_UNKNOWN` | `0x11` | Pi → STM32 | 有人脸但未注册 | 蜂鸣器告警 |
| `CMD_NOFACE` | `0x12` | Pi → STM32 | 画面中无人脸 | 忽略 |
| `CMD_MULTIFACE` | `0x13` | Pi → STM32 | 检测到多张人脸 | 蜂鸣器告警 |
| `CMD_HEARTBEAT` | `0x1F` | Pi → STM32 | 心跳 (每 5s) | 发送 ACK ("HB") |
| `CMD_ACK` | `0x20` | STM32 → Pi | 确认应答 | — |

**看门狗 (IWDG)：**

```
预分频器: 64 → 计数器: 1250 → 超时 ≈ 2 秒
软件定时器每 500ms 喂狗一次
防死机自恢复
```

---

## 三、端到端数据流

```
步骤  路径                       数据                             结果
═══════════════════════════════════════════════════════════════════════════════
 1    手机 → Windows :8081      照片 JPEG + 姓名                  User 记入 SQLite
                                 POST /api/register               照片存 photos/
                                                                  SyncTask 入队

 2    Windows → Pi :5000        照片 + 姓名                       C++ facerec (或 ONNX):
                                 POST /api/register               detect → align → extract
                                                                  features/<name>.bin

 3    Windows ← Pi :5000        已注册用户列表                    去重检查 (注册前)
                                 GET /api/features

 4    Pi 摄像头 → C++/ONNX      实时 BGR 画面                     SCRFD 检测
                                 (640×480 @15fps)                 → MobileFaceNet 512维
                                                                  → 余弦相似度匹配 (阈值 0.4)

 5    Pi → STM32 UART           识别结果 COBS+CRC8 帧            继电器开门
                                 0x10: 识别成功                   蜂鸣器提示
                                 0x11: 未注册人脸
                                 0x12: 无人脸
                                 0x1F: 心跳 (每 5s)

 6    Pi → Windows :8080        MJPEG 视频流                     管理端"实时画面"查看
                                 multipart/x-mixed-replace
```

---

## 四、项目结构

```
FaceRecognition/
├── Windows/                                # Windows 后台
│   ├── main.py                             # FastAPI 入口 + pywebview 桌面
│   ├── models.py                           # SQLAlchemy ORM (User/SyncTask/SystemStatus)
│   ├── sync.py                             # Pi 同步: worker 轮询 + 指数退避重试
│   ├── config.py                           # 全局配置 (端口/DB/Pi 地址)
│   ├── database.py                         # SQLAlchemy Session 依赖注入
│   ├── build_exe.bat                       # PyInstaller 打包 (→ dist/FaceRecognition.exe)
│   ├── frontend/                           # Vue 3 管理面板 SPA
│   │   └── src/
│   │       ├── api/index.ts                # Axios API 封装
│   │       ├── views/Dashboard.vue         # 仪表盘 (统计+Pi 状态)
│   │       ├── views/Users.vue             # 人员管理 (列表+同步+远程删除)
│   │       └── views/Settings.vue          # 系统设置 (Pi 配置)
│   ├── photos/                             # 用户照片存储
│   └── dist/                               # 打包输出 FaceRecognition.exe
│
├── Phone/                                  # 微信小程序
│   ├── project.config.json                 # 项目配置
│   └── miniprogram/pages/register/         # 注册页
│       ├── register.js                     # 拍照/选图/上传/扫码配置/连接测试
│       ├── register.wxml                   # 页面模板
│       └── register.wxss                   # 样式
│
├── Linux/                                  # 树莓派端
│   ├── Model/                              # ncnn 推理核心 + 注册 API
│   │   ├── inc/
│   │   │   ├── facedetector.h              # FaceDetector 声明
│   │   │   ├── facealigner.h               # FaceAligner 声明
│   │   │   └── featureextractor.h          # FeatureExtractor 声明
│   │   ├── src/
│   │   │   ├── facedetector.cpp            # SCRFD 检测器 (ncnn, vectorized decode, NMS)
│   │   │   ├── facealigner.cpp             # 5 点仿射对齐 (ArcFace 112×112)
│   │   │   ├── featureextractor.cpp        # MobileFaceNet 特征提取 (L2 归一化)
│   │   │   └── main.cpp                    # CLI + FeatureDB + Live webcam 模式
│   │   ├── models/
│   │   │   ├── ncnn_models/                # ncnn 格式模型 (.param + .bin)
│   │   │   │   ├── det_500m_dyn.*          # SCRFD 检测 (动态 Interp)
│   │   │   │   └── w600k_mbf.*             # MobileFaceNet
│   │   │   └── onnx_models/buffalo_s/      # ONNX 原始模型 (备份)
│   │   ├── pi_server.py                    # Flask 注册 API (:5000)
│   │   ├── features/                       # 人脸特征 .bin 存储
│   │   ├── CMakeLists.txt                  # CMake 构建 (ncnn + OpenCV)
│   │   └── benchmark.py                    # 性能基准评估
│   └── Raspberry Pi/                       # Python 运行时
│       ├── main.py                         # 多线程主入口
│       ├── config.json                     # 运行时配置
│       └── src/
│           ├── config.py                   # 配置 dataclass + 环境变量覆盖
│           ├── camera.py                   # 摄像头采集 + FrameQueue + ResultQueue
│           ├── recognition.py              # ONNX 识别引擎 (备选, 与 C++ 兼容)
│           ├── inference.py                # 推理线程 (跳帧+IOU跟踪+身份确认)
│           ├── feature_db.py               # 特征库 CRUD + 热加载 (inotify)
│           ├── uart.py                     # COBS+CRC8 帧协议 + 串口收发线程
│           └── mjpeg.py                    # MJPEG 视频流 Flask 服务 (:8080)
│
├── Stm32/                                  # STM32 固件
│   ├── Project.uvprojx                     # Keil MDK 工程
│   ├── User/main.c                         # FreeRTOS 入口 + 任务 + 定时器 + IWDG
│   ├── Comms/
│   │   ├── uart_dma.c/.h                   # DMA 循环模式 RX + 链式 TX + IDLE 中断
│   │   ├── uart_api.c/.h                   # 互斥锁保护的 UART 收发 API
│   │   ├── uart_port.c/.h                  # USART1 SPL 寄存器层
│   │   └── ringbuffer.c/.h                 # SPSC 无锁环形缓冲区
│   └── Protocol/
│       ├── frame_protocol.h                # COBS+CRC8 协议定义 + API
│       ├── frame_protocol.c                # 编解码 + CRC8 查表 + 状态机解析器
│       └── cmd_handler.c/.h                # 命令分发 + 各 CMD handler
│
├── tools/                                  # 辅助工具
│   ├── stm32_serial_test.py                # STM32 COBS 帧交互式测试 (6 种损坏场景)
│   ├── qr-generator.html                   # 服务器配置二维码生成器
│   └── uart_test.py                        # Pi 端串口发码测试
│
├── docs/                                   # 文档
└── CLAUDE.md                               # Git 编码规范 (Conventional Commits)
```

---

## 五、特征存储格式 (.bin)

**写入 (C++ FeatureDB::save / Python feature_db.save_one / pi_server._save_feature)：**

```
[4 bytes] dim (int32, little-endian)    → 固定 512
[N×4 bytes] feat[0..511] (float32 LE)  → 已 L2 归一化
```

**读取 (C++ FeatureDB::loadOne / Python feature_db._load_one)：**

```cpp
int32_t dim;  ifs.read(&dim, 4);
vector<float> feat(dim);  ifs.read(feat.data(), dim*4);
// feat 已经 L2 归一化, |feat| = 1.0
```

**匹配 (余弦相似度 = 点积)：**

```cpp
float dot = 0;
for (size_t i = 0; i < a.size(); i++) dot += a[i] * b[i];
return dot;  // 阈值 0.4
```

> C++ ncnn 引擎和 Python ONNX 引擎产生的 `.bin` **格式完全一致**，可互换使用。

---

## 六、快速开始

### Windows 后台

```bash
# 打包版 (推荐)
双击 dist/FaceRecognition.exe     # 服务 :8081, 管理面板自动弹出

# 开发版
cd Windows
conda activate facerec
python main.py
```

### 手机小程序

1. 微信开发者工具打开 `Phone/` 目录
2. 编译 → 预览 → 扫码
3. 设置服务器地址 → 拍照 → 输入姓名 → 上传

也可用 `tools/qr-generator.html` 生成服务器地址二维码, 打印贴在设备上供用户扫码。

### 树莓派

```bash
# ── 编译 C++ 推理引擎 ──
cd ~/Desktop/Model
mkdir -p build && cd build
cmake .. -DNCNN_DIR=/path/to/ncnn -DOpenCV_DIR=/path/to/opencv
make -j4
# 产物: build/facerec

# ── 启动注册 API 服务 (开机自启: sudo systemctl enable pi-face-api) ──
cd ~/Desktop/Model
~/miniforge3/envs/facerec/bin/python pi_server.py --port 5000

# ── 启动实时识别运行时 ──
cd ~/Desktop/Raspberry\ Pi
~/miniforge3/envs/facerec/bin/python main.py               # 完整模式
~/miniforge3/envs/facerec/bin/python main.py --no-uart      # 仅推理+视频 (调试)
~/miniforge3/envs/facerec/bin/python main.py --list-ports   # 查看可用串口
~/miniforge3/envs/facerec/bin/python main.py --debug        # 打印推理调试信息
```

> **引擎模式**: pi_server.py 优先调用 C++ `build/facerec`, 若未编译则自动回退到 ONNX 引擎。
> **环境管理**: 使用 miniforge conda 环境 `facerec` (Python 3.11 + opencv + flask + pyserial + onnxruntime)。

### STM32 固件

```bash
# 1. Keil MDK 打开 Stm32/Project.uvprojx → 编译 → 烧录
# 2. 测试: USB-TTL 连 PA9(RX)/PA10(TX)/GND
python tools/stm32_serial_test.py --port COM4 --interactive
```

### 硬件接线

```
树莓派 (40pin)              STM32F103
────────────────────────────────────────
GPIO14 (TXD, 物理 8 脚)  →  PA10 (USART1 RX)
GPIO15 (RXD, 物理 10 脚) →  PA9  (USART1 TX)
GND   (物理 6 脚)        →  GND

树莓派 (40pin)              USB-TTL (调试)
────────────────────────────────────────
GPIO14 (TXD, 物理 8 脚)  →  RX
GPIO15 (RXD, 物理 10 脚) →  TX
GND   (物理 6 脚)        →  GND
```

**Pi 串口配置 (前置步骤)：**
```bash
# 1. 启用 UART + 禁用蓝牙
echo "enable_uart=1" | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=disable-bt" | sudo tee -a /boot/firmware/config.txt

# 2. 移除串口控制台
sudo sed -i 's/console=serial0,115200 //' /boot/firmware/cmdline.txt

# 3. 禁用串口 Getty
sudo systemctl disable serial-getty@serial0.service
sudo systemctl disable serial-getty@ttyAMA0.service

# 4. 重启
sudo reboot
# 重启后 /dev/serial0 → /dev/ttyAMA0 可用
```

---

## 七、当前状态

| 模块 | 状态 | 说明 |
|------|------|------|
| 手机小程序 | ✅ 完成 | 拍照上传 + 扫码配置 + 连接测试 |
| Windows 后台 | ✅ 完成 | 管理面板 + Pi 同步 + 远程删除 + PyInstaller 打包 |
| Pi 注册 API | ✅ 完成 | Flask :5000, 调用 C++/ONNX 推理, 特征 CRUD |
| Pi 实时识别 | ⏳ 就绪 | 4 线程管线 + COBS/CRC8 UART + MJPEG (待摄像头+STM32 联调) |
| C++ ncnn 引擎 | ✅ 代码完成 | SCRFD + MobileFaceNet 全链路, 待 Pi 上 cmake 编译 |
| STM32 固件 | ✅ 完成 | FreeRTOS + DMA UART + COBS/CRC8 (待硬件联调) |

**待完成：**
- Pi 上 cmake 编译 C++/ncnn 引擎 (需安装 ncnn 库)
- Pi ↔ STM32 硬件接线 + 端到端联调
- 摄像头到货后实时识别 + UART 控制测试
- 识别日志回传 (Pi → Windows)
- 实际部署场景 FAR/FRR 测试
