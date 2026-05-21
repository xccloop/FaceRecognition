# FaceRecognition — 人脸识别门禁系统

> **作者**：向治昌  
> **定位**：四端协同的人脸识别门禁系统 — 手机采集人脸 → Windows 后台管理 → 树莓派人脸识别 → STM32 控制继电器开门

---

## 一、系统架构

```
┌─────────────────┐     HTTP/REST      ┌────────────────────┐     UART / 二进制帧      ┌──────────────┐
│   手机端（微信小程序） │ ────────────────→ │   Windows 后台（桌面）  │ ──────────────────────→ │  STM32 固件    │
│   人脸采集入口       │                  │   FastAPI + Vue 3    │                        │  继电器/门锁控制 │
└─────────────────┘                     └──────────┬─────────┘                        └──────────────┘
                                                   │
                                                   │ SSH/SCP
                                                   ▼
                                          ┌────────────────┐
                                          │  树莓派（Pi）    │
                                          │  人脸识别推理    │
                                          └────────────────┘
```

- **手机端**：微信小程序，拍照 + 填姓名 → 上传到 Windows 后台
- **Windows 后台**：FastAPI REST API + Vue 3 管理界面 + SQLite 数据库
- **STM32**：FreeRTOS 固件，通过 UART 接收命令控制继电器/门锁
- **树莓派**：运行人脸识别模型，接收 Windows 同步的照片特征数据

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
| 上传进度显示 | ✅ 完成 | Loading + 成功/失败提示 |

**进度**：功能完整，`register` 页面已可用。

### 2.2 Windows 后台（Windows/）— Python + Vue 3

| 功能 | 状态 | 说明 |
|------|------|------|
| FastAPI REST API | ✅ 完成 | 用户 CRUD、配置、注册、Pi 同步管理 |
| SQLite 数据库 | ✅ 完成 | SQLAlchemy ORM，User / SystemStatus 表 |
| Vue 3 管理界面 | ✅ 完成 | Element Plus UI，4 个页面 |
| Dashboard 仪表盘 | ✅ 完成 | 统计卡片（用户数/Pi在线/摄像头FPS） |
| 人员管理 | ✅ 完成 | 注册(照片+姓名)、搜索、列表、删除、照片预览、Pi 同步 |
| 实时画面 | ✅ 完成 | MJPEG 流展示，Pi 在线自动连接 |
| 系统配置 | ✅ 完成 | SSH 主机/端口/用户名/密码/特征目录配置，运行时持久化 |
| Pi 同步模块 | ✅ 完成 | SSH 检测 → SCP 传输照片到树莓派，支持单用户和批量同步 |
| PyInstaller 打包 | ✅ 完成 | 单文件 EXE（~31MB），双击即用，含前端内置 |
| 手机注册 API | ✅ 完成 | `POST /api/register` multipart 接收小程序上传 |

**进度**：Windows 端核心功能 100% 完成，v1.2.0 已发布 EXE。

### 2.3 STM32 固件（Stm32/）— C / FreeRTOS

| 功能 | 状态 | 说明 |
|------|------|------|
| FreeRTOS 多任务 | ✅ 完成 | 任务调度、软件定时器 |
| UART 二进制帧协议 | ✅ 完成 | 0xAA/0x55 帧头尾，0xBB 字节填充，XOR 校验 |
| LED 指示 | ✅ 完成 | 状态灯控制 |
| LCD 显示 | ✅ 完成 | 状态信息显示 |
| 继电器控制 | ✅ 完成 | GPIO 控制门锁 |
| 独立看门狗 (IWDG) | ✅ 完成 | 防死机自动复位 |
| 命令绑定 | ⚠️ 基础 | 协议解析完成，命令→动作映射待完善 |

**进度**：底层通信和驱动完成约 80%，上层命令逻辑待完善。

### 2.4 树莓派端（Linux/）— 人脸识别推理

| 功能 | 状态 | 说明 |
|------|------|------|
| 人脸检测/识别 | ⚠️ 待开发 | 目录存在但代码不完整 |
| 与 Windows 通信 | ⚠️ 待开发 | Windows 侧 SSH/SCP 已就绪 |

**进度**：约 10%，Windows 侧同步层已就绪，Pi 侧推理待开发。

---

## 三、技术栈

### Windows 后台
| 层级 | 技术 | 版本 |
|------|------|------|
| 语言 | Python | 3.x |
| Web 框架 | FastAPI | — |
| ASGI 服务器 | Uvicorn | — |
| ORM | SQLAlchemy | — |
| 数据库 | SQLite |
| 桌面窗口 | pywebview + Edge WebView2 | — |
| 前端框架 | Vue 3 + TypeScript | ^3.4 |
| 构建工具 | Vite | ^5.1 |
| UI 库 | Element Plus | ^2.5 |
| HTTP 客户端 | Axios | ^1.6 |
| Pi 通信 | paramiko (SSH/SCP) | — |
| 打包 | PyInstaller | — |

### 手机端（微信小程序）
| 层级 | 技术 | 说明 |
|------|------|------|
| 平台 | 微信小程序原生 | 无需 uni-app / Taro |
| 语言 | JavaScript (ES5) | 兼容低版本 iOS |
| UI | 原生组件 | 不引入第三方 UI 库 |
| 网络 | `wx.uploadFile` | multipart/form-data |
| 存储 | `wx.StorageSync` | 服务器配置持久化 |
| 基础库 | ≥3.3.4 | — |

### STM32 固件
| 层级 | 技术 | 说明 |
|------|------|------|
| MCU | STM32 | — |
| RTOS | FreeRTOS | 多任务 + 软件定时器 |
| 开发环境 | Keil MDK | — |
| 通信 | UART 自定义二进制帧协议 | — |
| 外设 | GPIO（继电器/LED）、LCD | — |

---

## 四、核心通信协议

### UART 二进制帧协议（Windows ↔ STM32）

```
帧格式: [0xAA] [CMD] [LEN] [PAYLOAD...] [XOR-CRC] [0x55]
         ↑                                        ↑
       帧头                                      帧尾

转义规则: 数据中出现 0xAA / 0x55 / 0xBB 时
         前插 0xBB 作为转义符
```

- **帧头**：`0xAA`（1 字节）
- **帧尾**：`0x55`（1 字节）
- **命令码**：1 字节
- **长度**：1 字节（负载长度）
- **校验**：1 字节（帧头到负载末尾的 XOR 和）
- **字节填充**：负载中 `0xAA`/`0x55`/`0xBB` 前插 `0xBB`

---

## 五、特殊适配说明

### 5.1 手机端 ES5 兼容
- **原因**：部分低版本 iOS 微信不支持 ES6 语法
- **措施**：全部使用 `function` 而非箭头函数，字符串拼接代替模板字符串，`var` 代替 `let/const`

### 5.2 隐私合规
- 微信要求调用摄像头前获取用户隐私授权
- 通过 `wx.requirePrivacyAuthorize` 实现

### 5.3 桌面窗口模式
- 使用 `pywebview` + Edge WebView2 实现原生桌面窗口
- 非浏览器模式，无地址栏和标签页
- PyInstaller 打包为单文件 EXE，双击即用

---

## 六、快速开始

### 6.1 下载 EXE（Windows 桌面端）

从 [GitHub Releases](https://github.com/xccloop/FaceRecognition/releases) 下载最新 `FaceRecognition.exe`，双击运行。

### 6.2 从源码启动 Windows 后台

```bash
cd Windows
pip install -r requirements.txt
cd frontend && npm install && npm run build && cd ..
python main.py
```

### 6.3 手机端使用

1. 微信开发者工具打开 `Phone/` 目录
2. 在设置中填入 Windows 后台的局域网 IP
3. 扫码预览，拍照上传测试

---

## 七、项目结构

```
FaceRecognition/
├── Windows/                       # Windows 后台（Python + Vue 3）
│   ├── README.md                  # Windows 端文档
│   ├── main.py                    # FastAPI 主入口 + webview
│   ├── models.py                  # SQLAlchemy 数据模型
│   ├── config.py                  # Pydantic 配置管理
│   ├── sync.py                    # Pi SSH/SCP 同步模块
│   ├── database.py                # 数据库会话依赖
│   ├── FaceRecognition.spec       # PyInstaller 打包配置
│   ├── requirements.txt           # Python 依赖
│   ├── dist/                      # 打包输出（FaceRecognition.exe）
│   ├── photos/                    # 人员照片存储
│   └── frontend/                  # Vue 3 前端
│       ├── package.json
│       ├── vite.config.ts
│       └── src/
│           ├── App.vue
│           ├── router/
│           ├── api/
│           └── views/
│               ├── Dashboard.vue   # 仪表盘
│               ├── Users.vue       # 人员管理
│               ├── Camera.vue      # 实时画面
│               └── Settings.vue    # 系统配置
├── Phone/                         # 手机端（微信小程序）
│   └── miniprogram/
├── Stm32/                         # STM32 固件
├── Linux/                         # 树莓派端（待开发）
├── AGENT.md                       # Git 工作流规范
└── README.md                      # 本文件
```

---

## 八、Git 工作流

本项目使用严格的分支开发策略（详见 `AGENT.md`）：

- **永远不在 main 上直接开发**，所有改动通过功能分支
- **Commit Message** 遵循 Conventional Commits
- **分支命名**：`feat/<描述>` / `fix/<描述>` / `docs/<描述>`
- **合并方式**：Squash and Merge

---

## 九、待完成

| 优先级 | 模块 | 任务 |
|--------|------|------|
| P0 | STM32 | 命令→继电器动作映射完善 |
| P1 | Linux | 树莓派人脸识别推理模块 |
| P1 | 全系统 | 端到端集成测试（手机→Windows→STM32） |
| P2 | Windows | 登录认证（如需公网部署） |
| P2 | Phone | 生产环境部署（HTTPS + 域名白名单） |

---

## 十、许可证

MIT License — 作者：向治昌
