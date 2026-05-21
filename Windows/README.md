# 人脸识别门禁系统 — Windows 桌面管理端

> 作者：向治昌  
> 版本：v1.2.0  
> 定位：人脸识别门禁系统的 Windows 侧桌面管理工具，负责人员注册、照片管理、树莓派同步和实时视频监控。

---

## 一、功能概览

| 功能 | 说明 |
|------|------|
| 仪表盘 | 总人数统计、Pi 在线状态、摄像头 FPS 实时展示 |
| 人员管理 | 注册（照片+姓名）、列表查看、分页搜索、删除、照片预览、Pi 同步状态 |
| 实时画面 | 连接树莓派后显示 MJPEG 实时视频流（320x240） |
| 系统配置 | SSH 连接配置（主机/端口/用户名/密码/特征目录） |
| Pi 同步 | SSH 检测 Pi 在线 → SCP 传输照片到树莓派特征目录，支持单用户和批量同步 |
| 手机注册支持 | 开放 REST API，手机小程序可上传照片完成注册 |

---

## 二、技术栈

| 层级 | 技术 |
|------|------|
| 后端框架 | FastAPI (Python) |
| ASGI | Uvicorn |
| ORM | SQLAlchemy + SQLite |
| 前端 | Vue 3 + TypeScript + Vite |
| UI 库 | Element Plus |
| 桌面窗口 | pywebview + Edge WebView2 |
| Pi 通信 | SSH/SCP（paramiko） |
| 打包 | PyInstaller（单文件 EXE，~31MB） |

---

## 三、快速开始

### 3.1 下载 EXE（推荐）

从 [GitHub Releases](https://github.com/xccloop/FaceRecognition/releases) 下载最新 `FaceRecognition.exe`，双击运行即可。

> EXE 会解压临时文件到系统临时目录，用户数据（照片、数据库、配置）保存在 EXE 同级目录下。

### 3.2 从源码运行

```bash
# 1. 安装依赖
cd Windows
pip install -r requirements.txt

# 2. 构建前端（可选，首次运行需构建）
cd frontend && npm install && npm run build && cd ..

# 3. 启动
python main.py
```

### 3.3 开发模式

```bash
# 后端
cd Windows && python main.py

# 前端热重载
cd Windows/frontend && npm run dev
# Vite 开发服务器运行在 http://localhost:5173，自动代理 API 到 8000
```

---

## 四、API 端点

### 管理面板 API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/dashboard` | 仪表盘数据（用户数、Pi 状态） |
| GET | `/api/users?page=1&page_size=12&search=` | 人员列表（分页+搜索） |
| DELETE | `/api/users/{id}` | 删除人员及照片 |
| GET | `/api/camera/status` | 摄像头状态 + MJPEG 流地址 |
| GET | `/api/config` | 获取系统配置 |
| PUT | `/api/config` | 保存系统配置 |
| GET | `/api/pi/status` | 树莓派在线状态（实时 SSH 检测） |
| POST | `/api/users/{id}/sync` | 同步单个用户到 Pi |
| POST | `/api/users/sync-all` | 批量同步所有未同步用户 |

### 手机注册 API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/register` | 人脸注册（multipart: photo + name） |

---

## 五、配置说明

系统配置通过 Settings 页面或 `config.json` 管理，运行时自动持久化：

```json
{
  "pi_host": "192.168.1.100",
  "pi_stream_port": 8080,
  "pi_ssh_port": 22,
  "pi_user": "pi",
  "pi_password": "",
  "pi_features_dir": "/home/pi/features/",
  "server_port": 8000,
  "server_host": "0.0.0.0",
  "heartbeat_timeout": 15
}
```

配置修改后在 UI 点击保存即可，部分参数（如 server_port）需重启生效。

---

## 六、目录结构

```
Windows/
├── main.py                 # FastAPI 主入口 + pywebview 窗口
├── models.py               # SQLAlchemy 数据模型
├── config.py               # 配置管理
├── sync.py                 # Pi SSH/SCP 同步模块
├── database.py             # 数据库会话
├── FaceRecognition.spec    # PyInstaller 打包配置
├── requirements.txt        # Python 依赖
├── frontend/
│   ├── package.json
│   ├── vite.config.ts
│   └── src/
│       ├── App.vue
│       ├── router/
│       ├── api/            # Axios API 封装
│       └── views/
│           ├── Dashboard.vue   # 仪表盘
│           ├── Users.vue       # 人员管理
│           ├── Camera.vue      # 实时画面
│           └── Settings.vue    # 系统配置
├── photos/                 # 人员照片存储目录
├── dist/                   # PyInstaller 打包输出
│   ├── FaceRecognition.exe # 可执行文件
│   ├── face_recognition.db # SQLite 数据库（运行时生成）
│   ├── config.json         # 运行时配置
│   └── photos/             # 用户照片
└── frontend/dist/          # 前端构建产物
```

---

## 七、Pi 同步流程

```
1. 用户在 Windows 端注册 / 手机小程序上传照片
2. 系统自动保存照片到 photos/ 目录
3. 配置 Pi SSH 连接信息（设置页面）
4. 手动或自动触发同步 → sync.py 通过 SCP 将照片传输到 Pi 的 /home/pi/features/
5. Pi 端运行人脸识别推理时读取 features/ 目录
```

---

## 八、打包为 EXE

```bash
# 1. 构建前端
cd frontend && npm run build && cd ..

# 2. 打包
pyinstaller FaceRecognition.spec

# 3. 产物在 dist/FaceRecognition.exe（~31MB）
```

打包后 EXE 同级目录需包含编译好的前端文件（已由 `.spec` 配置自动包含）。

---

## 九、更新日志

### v1.2.0 — 2025-05-21

**Fixed**
- 修复 PyInstaller 打包后人员照片不显示的问题（StaticFiles `/photos` 路径不同步导致 404）

### v1.1.5
- 人员管理页面支持上传照片预览
- 系统配置支持 SSH/SCP 参数保存
- Dashboard 实时统计 + Pi 在线检测

---

## 十、许可证

MIT License — 作者：向治昌
