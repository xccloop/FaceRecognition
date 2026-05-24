# FaceRecognition — 人脸识别门禁系统

> **作者**：向治昌
> **定位**：四端协同的人脸识别门禁系统 — 手机采集人脸 → Windows 后台管理 → 树莓派人脸识别 → STM32 控制继电器开门

---

## 一、系统架构

```
                      HTTP 远程注册
  ┌──────────────┐  (照片+姓名)   ┌───────────────────────┐
  │ 手机端        │ ────────────→ │  Windows 后台（桌面）    │
  │ 微信小程序     │              │  FastAPI + Vue 3       │
  │ 人脸采集入口   │              │  SQLite + 管理面板      │
  └──────────────┘              │  端口 :8000            │
                                └──────────┬────────────┘
                                           │
                         ┌─────────────────┴─────────────────┐
                         │ HTTP                            HTTP│
                         │ POST 注册同步                    MJPEG│
                         │ (pi_server :5000)          视频流回传 │
                         ▼                                  ▼
                   ┌──────────────────────────────────────────┐
                   │           树莓派 4B（核心推理引擎）         │
                   │                                          │
                   │  ┌─────────────┐  ┌──────────────────┐   │
                   │  │ facerec     │  │ pi_server.py     │   │
                   │  │ (C++/ncnn)  │  │ (Flask :5000)    │   │
                   │  │ · 摄像头采集 │  │ · 接收远程注册    │   │
                   │  │ · SCRFD 检测 │  │ · 调 facerec CLI │   │
                   │  │ · 特征提取   │  └──────────────────┘   │
                   │  │ · 特征库匹配 │                         │
                   │  └──────┬──────┘                         │
                   │         │ UART / 二进制帧                 │
                   │         │ 115200 8-N-1                   │
                   └─────────┼────────────────────────────────┘
                             │
                   ┌─────────▼────────────────────────────────┐
                   │         STM32F103C8T6 (FreeRTOS)        │
                   │         · DMA+RingBuffer UART 接收       │
                   │         · 帧协议解析 + 命令分发           │
                   │         · 继电器控制 / 蜂鸣器 / LED       │
                   └──────────────────────────────────────────┘
```

**数据流**：
1. **手机 → Windows**：微信小程序拍照填姓名，HTTP POST 上传到 Windows 后台
2. **Windows → 树莓派**：后台将照片通过 HTTP POST 发到 Pi 的 Flask API (`pi_server.py:5000`)，Pi 调用 C++ 引擎完成注册
3. **树莓派 → Windows**：MJPEG 视频流回传，Windows 管理端"实时画面"页面查看
4. **树莓派 → STM32**：识别结果通过 UART 串口发送二进制命令帧，STM32 解析后控制继电器开门

---

## 二、各模块完成情况

### 2.1 手机端（Phone/）— 微信小程序

| 功能 | 状态 | 说明 |
|------|------|------|
| 拍照采集 | ✅ 完成 | `wx.chooseMedia` 调用系统相机 |
| 从相册选照片 | ✅ 完成 | 支持相册选取 |
| 姓名输入 | ✅ 完成 | 表单输入 + 前端校验 |
| 上传到后台 | ✅ 完成 | `wx.uploadFile` multipart/form-data → `POST /api/register` |
| 服务器配置 | ✅ 完成 | 可配置 Windows 后台 IP + 端口，带连接测试 |
| 隐私授权 | ✅ 完成 | `wx.requirePrivacyAuthorize` 合规处理 |
| ES5 兼容 | ✅ 完成 | 全链路 ES5 语法，兼容低版本 iOS 微信 |

**进度**：MVP 100% 完成，单页注册流程端到端可用。

### 2.2 Windows 后台（Windows/）— Python + Vue 3

| 功能 | 状态 | 说明 |
|------|------|------|
| FastAPI REST API | ✅ 完成 | 用户 CRUD、配置、注册、Pi 同步管理 |
| SQLite 数据库 | ✅ 完成 | SQLAlchemy ORM，User / SystemStatus / RecogLog 表 |
| Vue 3 管理界面 | ✅ 完成 | Element Plus UI，4 个页面 |
| Dashboard 仪表盘 | ✅ 完成 | 统计卡片（用户数/Pi在线/摄像头FPS） |
| 人员管理 | ✅ 完成 | 注册(照片+姓名)、搜索、列表、删除、照片预览、Pi 同步 |
| 实时画面 | ✅ 完成 | MJPEG 流展示，Pi 在线自动连接 |
| 系统配置 | ✅ 完成 | Pi HTTP API 配置（主机/端口），运行时持久化 |
| Pi 同步模块 | ✅ 完成 | HTTP API 检测 → POST 照片到 Pi 注册，支持单用户和批量同步 |
| PyInstaller 打包 | ✅ 完成 | 单文件 EXE（~31MB），双击即用，含前端内置 |
| 手机注册 API | ✅ 完成 | `POST /api/register` multipart 接收小程序上传 |

**进度**：v1.2.0 核心功能 100% 完成。

### 2.3 树莓派端（Linux/）— C++/ncnn 推理引擎

| 功能 | 状态 | 说明 |
|------|------|------|
| 人脸检测 (SCRFD) | ✅ 完成 | 动态 Interp 模型，max-side=480 缩放，匹配 Python 精度 |
| 人脸对齐 | ✅ 完成 | 5 点仿射变换，ArcFace 112×112 模板 |
| 特征提取 (MobileFaceNet) | ✅ 完成 | L2 归一化，512 维特征向量 |
| CLI 命令 | ✅ 完成 | register / identify / compare / test / live |
| 实时摄像头 | ✅ 完成 | 实时识别 + HUD + 多帧注册 + 热键 (Q/R/I/S) |
| 特征库 (FeatureDB) | ✅ 完成 | 二进制 .bin 格式，与 Python 互操作 |
| 注册 API 服务 | ✅ 完成 | Flask HTTP :5000，接收 Windows 远程注册 |
| 系统部署 | ✅ 完成 | systemd 开机自启，一键安装脚本 |
| UART 串口通信 | ⚠️ 待实现 | Python 版已有，C++ 版待开发 |
| MJPEG 视频流 | ⚠️ 待实现 | 依赖 Python 版 mjpeg.py |

**进度**：C++/ncnn 核心推理 95% 完成，识别精度已对齐 Python/ONNX。详见 `Linux/Model/doc/ncnn-alignment.md`。

### 2.4 STM32 固件（Stm32/）— C / FreeRTOS

| 功能 | 状态 | 说明 |
|------|------|------|
| FreeRTOS 多任务 | ✅ 完成 | 任务调度（静态内存分配）、软件定时器 |
| UART DMA+RingBuffer | ✅ 完成 | USART1, DMA1 循环接收 + RingBuffer 缓冲 + 任务通知 |
| 二进制帧协议 | ✅ 完成 | 0xAA/0x55 帧头尾, 0xBB 字节填充, XOR 校验, 变长数据段 |
| LED 指示 | ✅ 完成 | PB5 + PE5 双 LED |
| 独立看门狗 (IWDG) | ✅ 完成 | ~2s 超时, 500ms 喂狗定时器 |
| 心跳超时检测 | ✅ 完成 | 15 秒超时告警, 恢复自动清除 |
| 命令分发 | ✅ 完成 | 5 条命令——识别成功/未注册/无人脸/多人脸/心跳 |
| 代码模块化重构 | ✅ 完成 | Comms/ + Protocol/ + User/ 目录拆分 |
| 继电器控制 | ⚠️ 待实现 | 需配合硬件接入 |
| 树莓派联调 | ⚠️ 待联调 | Pi 端 UART 就绪后对接 |

**进度**：底层通信和协议 100% 完成，PC 端联调通过。

---

## 三、技术栈

### Windows 后台
| 层级 | 技术 |
|------|------|
| 语言 | Python 3.x |
| Web 框架 | FastAPI + Uvicorn |
| ORM | SQLAlchemy + SQLite |
| 桌面窗口 | pywebview + Edge WebView2 |
| 前端 | Vue 3 + TypeScript + Vite + Element Plus |
| 打包 | PyInstaller |

### 树莓派端
| 层级 | 技术 |
|------|------|
| 推理引擎 | ncnn (ARM NEON 优化) |
| 视觉库 | OpenCV 4.x |
| 检测模型 | SCRFD det_500m (动态 Interp) |
| 识别模型 | MobileFaceNet w600k_mbf |
| 编译 | CMake + gcc |
| 部署 | systemd 开机自启 |
| 注册 API | Python Flask |

### 手机端（微信小程序）
| 层级 | 技术 |
|------|------|
| 平台 | 微信小程序原生 |
| 语言 | JavaScript (ES5) |
| UI | 原生组件 |
| 基础库 | ≥3.3.4 |

### STM32 固件
| 层级 | 技术 |
|------|------|
| MCU | STM32F103C8T6 (Cortex-M3, 64KB SRAM, 128KB Flash) |
| RTOS | FreeRTOS v10+ (静态内存分配) |
| 开发环境 | Keil MDK-ARM v5 |
| 标准库 | SPL (FWlib 3.5) |
| UART | USART1, DMA1 循环模式 + RingBuffer + IDLE 中断, 115200-8-N-1 |
| 协议 | 自定义二进制变长帧 (0xAA/0x55 帧边界, 0xBB 转义, XOR 校验) |

---

## 四、核心通信协议

### UART 二进制帧协议（Pi ↔ STM32）

```
帧格式: | 0xAA | CMD | DIR | LEN_H | LEN_L | DATA(转义)... | XOR | 0x55 |
         帧头   命令   方向   大端长度字段      变长数据段      校验   帧尾

转义规则 (仅 DATA 段):
  0xAA → 0xBB 0x55    0x55 → 0xBB 0xAA    0xBB → 0xBB 0x44

XOR = CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
(帧头 0xAA 和帧尾 0x55 不参与 XOR)
```

**命令字 (Pi → STM32)：**

| CMD | 常量 | 含义 | STM32 回复 |
|-----|------|------|-----------|
| 0x1F | HEARTBEAT | 心跳检测 | CMD_ACK(0x20) + "HB" |
| 0x10 | IDENTIFY | 识别成功(有人) | CMD_ACK(0x20) + 文本确认 |
| 0x11 | UNKNOWN | 有人脸未识别 | 文本告警 |
| 0x12 | NOFACE | 无人脸 | 文本提示 |
| 0x13 | MULTIFACE | 多人脸 | 文本告警 |

### HTTP 远程注册协议（Windows → Pi）

```
Windows 管理端 ──POST /api/register──→ pi_server.py:5000 (Flask)
  multipart/form-data: photo (file) + name (string)
                                      │
                                      ├── 保存照片到临时文件
                                      ├── 调用 facerec register <temp> <name>
                                      ├── 特征存入 features/<name>.bin
                                      └── 返回 JSON {success, face_score, feature_dim}
```

---

## 五、快速开始

### Windows 后台

从 [GitHub Releases](https://github.com/xccloop/FaceRecognition/releases) 下载 `FaceRecognition.exe`，双击运行。

或从源码：
```bash
cd Windows
pip install -r requirements.txt
cd frontend && npm install && npm run build && cd ..
python main.py
```

### 树莓派部署

```bash
# 1. 安装依赖 + 编译 ncnn + 编译 facerec + 安装服务
cd Linux/"Raspberry Pi"
chmod +x scripts/install.sh
./scripts/install.sh

# 2. 传输模型文件
# scp Linux/Model/models/ncnn_models/*.param pi@<IP>:~/facerec/Model/models/ncnn_models/
# scp Linux/Model/models/ncnn_models/*.bin   pi@<IP>:~/facerec/Model/models/ncnn_models/

# 3. 启动服务
sudo systemctl start face-recog face-register

# 4. 查看状态
sudo systemctl status face-recog
tail -f /var/log/face-recog.log
```

详见 `Linux/doc/树莓派部署指南-CPP.md`。

### STM32 固件

1. Keil MDK-ARM 打开 `Stm32/Project.uvprojx`
2. 编译 (F7) → 烧录 (F8)
3. USB-TTL 连接：TX→PA10, RX→PA9, GND→GND

### 手机端

1. 微信开发者工具打开 `Phone/` 目录
2. 在设置中填入 Windows 后台的局域网 IP
3. 扫码预览，拍照上传测试

---

## 六、项目结构

```
FaceRecognition/
├── Windows/                       # Windows 后台（Python + Vue 3）
│   ├── README.md
│   ├── main.py                    # FastAPI 主入口 + webview
│   ├── models.py / config.py / sync.py / database.py
│   ├── frontend/src/views/        # Dashboard / Users / Camera / Settings
│   └── photos/                    # 人员照片存储
├── Linux/
│   ├── Model/                     # C++/ncnn 推理引擎 ★
│   │   ├── src/                   # facedetector / facealigner / featureextractor / main
│   │   ├── inc/                   # 头文件
│   │   ├── models/ncnn_models/    # ncnn 模型 (det_500m_dyn + w600k_mbf)
│   │   ├── features/              # 注册的人脸特征 (.bin)
│   │   ├── pi_server.py           # Flask 注册 API (:5000)
│   │   ├── verify.py              # Python/ONNX 参考实现
│   │   └── doc/                   # 推理文档 + 精度对齐 + 部署指南
│   └── Raspberry Pi/              # Python 运行时（备选方案）
│       ├── main.py / config.json
│       ├── src/                   # camera / recognition / inference / uart / mjpeg
│       └── scripts/               # install.sh / face-recog.service
├── Phone/                         # 手机端（微信小程序）
│   └── miniprogram/
│       ├── app.js / app.json
│       ├── pages/register/        # 注册页（拍照+姓名+上传）
│       └── utils/api.js           # API 封装
├── Stm32/                         # STM32 固件
│   ├── Project.uvprojx            # Keil 工程
│   ├── User/                      # main.c + 中断服务
│   ├── Comms/                     # DMA+RingBuffer 通信框架
│   ├── Protocol/                  # 帧协议 + 命令处理
│   ├── FreeRTOS/ / Fwlib/
│   └── doc/
├── docs/                          # 项目总体方案
├── CLAUDE.md                      # Git 工作流规范
└── README.md                      # 本文件
```

---

## 七、Git 工作流

- **永远不在 main 上直接开发**，所有改动通过功能分支
- **Commit Message** 遵循 Conventional Commits：`<type>(<scope>): <subject>`
- **分支命名**：`feat/<描述>` / `fix/<描述>` / `docs/<描述>`
- **合并方式**：Squash and Merge

---

## 八、待完成

| 优先级 | 模块 | 任务 |
|--------|------|------|
| P0 | 全系统 | 树莓派 ↔ STM32 硬件接线 + 端到端联调 |
| P0 | STM32 | 继电器 GPIO 控制 + 开门时序 |
| P1 | Linux | 树莓派原生编译 + ARM NEON 性能测试 |
| P1 | Linux | Python UART/MJPEG 模块与 C++ 引擎联调 |
| P2 | STM32 | 蜂鸣器/按键/OLED 等外设扩展 |
| P2 | Windows | 识别日志回传（Pi → Windows） |

---

## 九、许可证

MIT License — 作者：向治昌
