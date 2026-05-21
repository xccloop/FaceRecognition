# FaceRecognition — 人脸识别门禁系统

> **作者**：向治昌  
> **定位**：三端协同的人脸识别门禁系统 — 手机采集人脸 → Windows 后台管理 → STM32 控制继电器开门

---

## 一、系统架构

```
┌─────────────────┐     HTTP/REST      ┌────────────────────┐     UART / 二进制帧      ┌──────────────┐
│   手机端（微信小程序） │ ────────────────→ │   Windows 后台（桌面）  │ ──────────────────────→ │  STM32 固件    │
│   人脸采集入口       │                  │   FastAPI + Vue 3    │                        │  继电器/门锁控制 │
└─────────────────┘                     └──────────┬─────────┘                        └──────────────┘
                                                   │
                                                   │ TCP（可选）
                                                   ▼
                                          ┌────────────────┐
                                          │  树莓派（Pi）    │
                                          │  人脸识别推理    │
                                          └────────────────┘
```

- **手机端**：微信小程序，拍照 + 填姓名 → 上传到 Windows 后台
- **Windows 后台**：FastAPI REST API + Vue 3 管理界面 + SQLite 数据库
- **STM32**：FreeRTOS 固件，通过 UART 接收命令控制继电器/门锁
- **树莓派**（可选）：运行人脸识别模型，通过 TCP 与 Windows 通信

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
| FastAPI REST API | ✅ 完成 | 用户 CRUD、日志、配置、注册、健康检查 |
| SQLite 数据库 | ✅ 完成 | SQLAlchemy ORM，User / LogEntry / SyncState 三表 |
| Vue 3 管理界面 | ✅ 完成 | Element Plus UI，4 个页面 |
| Dashboard 仪表盘 | ✅ 完成 | 统计卡片（用户数/今日识别/在线状态），countup.js 动画 |
| 人员管理 | ✅ 完成 | 搜索、列表、增删改、同步到 Pi |
| 摄像头预览 | ⚠️ 基础 | MJPEG 流代理，在线/离线状态显示 |
| 系统设置 | ✅ 完成 | 树莓派 IP/端口配置、服务器参数、运行时持久化 |
| Pi 同步模块 | ✅ 完成 | TCP 二进制帧协议，心跳保活，照片同步调度 |
| 登录认证 | ⚠️ 占位 | 前端登录页存在但后端无鉴权（内网使用） |
| PyInstaller 打包 | ⚠️ 待测 | 有 hook-webview.py，未完整测试打包 |

**进度**：核心功能完成约 85%，摄像头和登录模块待完善。

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
| 与 Windows TCP 通信 | ⚠️ 待开发 | sync.py 中 Windows 侧已就绪 |

**进度**：约 10%，Windows 侧通信层已就绪，Pi 侧推理待开发。

---

## 三、技术栈

### Windows 后台
| 层级 | 技术 | 版本 |
|------|------|------|
| 语言 | Python | 3.x |
| Web 框架 | FastAPI | — |
| ASGI 服务器 | Uvicorn | — |
| ORM | SQLAlchemy | — |
| 数据库 | SQLite（可切换） | — |
| 配置管理 | Pydantic Settings | — |
| 桌面窗口 | pywebview + Edge WebView2 | — |
| 前端框架 | Vue 3 + TypeScript | ^3.4 |
| 构建工具 | Vite | ^5.1 |
| UI 库 | Element Plus | ^2.5 |
| HTTP 客户端 | Axios | ^1.6 |
| 动画 | @vueuse/motion + countup.js | — |
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
| MCU | STM32（具体型号见工程配置） | — |
| RTOS | FreeRTOS | 多任务 + 软件定时器 |
| 开发环境 | Keil MDK (Project.uvprojx) | — |
| 通信 | UART 自定义二进制帧协议 | 见下方协议说明 |
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
- **命令码**：1 字节（如继电器开/关、状态查询等）
- **长度**：1 字节（负载长度）
- **校验**：1 字节（帧头~负载的 XOR 和）
- **字节填充**：负载中 `0xAA`/`0x55`/`0xBB` 前插 `0xBB`

### HTTP API（手机 ↔ Windows）

```
POST /api/register    — 人脸注册（multipart: photo + name）
GET  /api/users       — 人员列表
POST /api/users       — 添加用户
PUT  /api/users/{id}  — 更新用户
DELETE /api/users/{id} — 删除用户
GET  /api/dashboard/stats — 仪表盘统计
POST /api/camera/start    — 启动摄像头流
POST /api/camera/stop     — 停止摄像头流
GET  /api/config          — 获取配置
PUT  /api/config          — 更新配置
POST /api/sync/start      — 启动 Pi 同步
POST /api/sync/stop       — 停止 Pi 同步
```

---

## 五、特殊适配说明

### 5.1 手机端 ES5 兼容
- **原因**：部分低版本 iOS 微信不支持 ES6 语法
- **措施**：
  - 全部使用 `function` 而非箭头函数
  - 字符串拼接代替模板字符串
  - `var` 代替 `let/const`
  - `project.config.json` 设置 `"es6": false`、`"minified": false`、`"disableSWC": true`
  - 移除 `lazyCodeLoading` 避免远程编译兼容问题

### 5.2 隐私合规
- 微信要求调用摄像头前获取用户隐私授权
- 通过 `wx.requirePrivacyAuthorize` 实现
- `app.json` 中 `"__usePrivacyCheck__": true`

### 5.3 服务器配置灵活性
- 小程序端可通过设置弹窗配置 Windows 后台 IP 和端口
- 配置持久化到 `wx.StorageSync`
- 默认回退地址：`http://192.168.1.5:8000`

### 5.4 数据库可切换
- 默认使用 SQLite（零配置，适合桌面应用）
- 通过 SQLAlchemy 抽象，可切换至 MySQL / PostgreSQL
- 配置通过 Pydantic Settings 从 `config.yaml` 或环境变量读取

### 5.5 桌面窗口模式
- 使用 `pywebview` + Edge WebView2 实现原生桌面窗口
- 非浏览器模式，无地址栏和标签页
- PyInstaller 提供 `hook-webview.py` 运行时钩子

---

## 六、快速开始

### 6.1 启动 Windows 后台

```bash
cd Windows
pip install -r requirements.txt
python main.py
```

服务器启动后自动打开桌面窗口，访问 `http://127.0.0.1:8000`。

### 6.2 前端开发模式

```bash
cd Windows/frontend
npm install
npm run dev          # Vite 开发服务器，自动代理 API 到 :8000
```

### 6.3 手机端使用

1. 微信开发者工具打开 `Phone/` 目录
2. 在设置中填入 Windows 后台的局域网 IP
3. 扫码预览，拍照上传测试

### 6.4 STM32 固件烧录

Keil MDK 打开 `Stm32/Project.uvprojx`，编译后通过 ST-Link 烧录。

---

## 七、项目结构

```
FaceRecognition/
├── Windows/                    # Windows 后台（Python + Vue 3）
│   ├── main.py                 # FastAPI 主入口 + webview
│   ├── models.py               # SQLAlchemy 数据模型
│   ├── config.py               # Pydantic 配置管理
│   ├── sync.py                 # Pi 同步模块（TCP + 二进制帧）
│   ├── database.py             # 数据库会话依赖
│   ├── requirements.txt        # Python 依赖
│   ├── hook-webview.py         # PyInstaller 钩子
│   └── frontend/               # Vue 3 前端
│       ├── package.json
│       ├── vite.config.ts
│       ├── tsconfig.json
│       └── src/
│           ├── App.vue         # 根布局
│           ├── router/         # Vue Router
│           ├── api/            # Axios API 封装
│           ├── components/     # 公共组件（侧边栏）
│           └── views/          # 4 个页面
│               ├── Dashboard.vue   # 仪表盘
│               ├── Users.vue       # 人员管理
│               ├── Camera.vue      # 摄像头预览
│               └── Settings.vue    # 系统设置
├── Phone/                      # 手机端（微信小程序）
│   ├── miniprogram/
│   │   ├── app.js / app.json
│   │   ├── pages/register/     # 注册页面（唯一页面）
│   │   └── utils/api.js        # API 封装
│   ├── project.config.json
│   └── doc/
│       ├── ARCHITECTURE.md     # 系统架构文档
│       ├── tasks.md            # 任务分解与进度
│       └── DEVELOPMENT_ISSUES.md # 开发问题记录
├── Stm32/                      # STM32 固件
│   ├── User/main.c             # 主程序（FreeRTOS + UART 协议）
│   └── Project.uvprojx         # Keil 工程文件
├── Linux/                      # 树莓派端（待开发）
├── CLAUDE.md                   # Git 工作流规范
└── README.md                   # 本文件
```

---

## 八、Git 工作流

本项目使用严格的分支开发策略（详见 `CLAUDE.md`）：

- **永远不在 main 上直接开发**，所有改动通过功能分支
- **Commit Message** 遵循 Conventional Commits：
  - `feat(windows):` / `fix(stm32):` / `docs(project):` / `refactor(pi):`
- **分支命名**：`feat/<描述>` / `fix/<描述>` / `docs/<描述>` / `refactor/<描述>`
- **合并方式**：Squash and Merge

---

## 九、待完成

| 优先级 | 模块 | 任务 |
|--------|------|------|
| P0 | Windows | 完善摄像头预览功能 |
| P0 | STM32 | 命令→继电器动作映射完善 |
| P1 | Windows | 打包为独立 EXE（PyInstaller） |
| P1 | 全系统 | 端到端集成测试（手机→Windows→STM32） |
| P2 | Linux | 树莓派人脸识别推理模块 |
| P2 | Windows | 登录认证（如需公网部署） |
| P3 | Phone | 生产环境部署（HTTPS + 域名白名单） |

---

## 十、许可证

MIT License — 作者：向治昌
