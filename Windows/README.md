# 人脸识别门禁系统 — Windows 桌面管理端

> 作者：向治昌

## 技术栈

| 层 | 技术 |
|----|------|
| 后端框架 | Python 3.12, FastAPI + Uvicorn |
| ORM | SQLAlchemy + SQLite |
| 前端 | Vue 3 + TypeScript + Vite + Element Plus |
| 桌面壳 | pywebview (Edge WebView2) |
| 打包 | PyInstaller (单文件 EXE, ~34MB) |
| Pi 通信 | HTTP REST API (注册同步), MJPEG (视频流) |

## 功能

- 接收手机小程序注册请求 (POST `/api/register`)
- 异步同步用户照片+特征到树莓派 (持久化重试队列)
- 管理面板：仪表盘、人员列表、摄像头实时画面、系统设置
- 运行时配置持久化 (`config.json`)

## 目录结构

```
Windows/
├── main.py                       # FastAPI 入口 + pywebview 桌面窗口
├── config.py                     # 全局配置 + 环境变量
├── models.py                     # SQLAlchemy ORM (User, SyncTask, SystemStatus)
├── database.py                   # 数据库会话工厂
├── sync.py                       # Pi 同步模块 (HTTP API, 持久化重试队列)
├── requirements.txt              # Python 依赖
├── FaceRecognition.spec          # PyInstaller 打包配置
├── build_exe.bat                 # 一键打包脚本
├── config.json                   # 运行时配置 (自动生成)
├── frontend/
│   ├── src/                      # Vue 3 源码
│   │   └── views/                # Camera, Dashboard, Settings, Users
│   └── dist/                     # Vite 构建产物
├── photos/                       # 用户照片存储
└── dist/                         # PyInstaller 输出 (FaceRecognition.exe)
```

## 开发

```bash
cd Windows
pip install -r requirements.txt
python main.py                    # 服务 :8081, 管理面板自动弹出
```

## 打包

```bash
build_exe.bat                     # → dist/FaceRecognition.exe (~34MB)
```

需要 `conda env facerec` (或手动安装 pywebview + pythonnet)。

## API 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/register` | 手机注册 (multipart: photo + name) |
| GET | `/api/dashboard` | 仪表盘数据 (人数/在线状态) |
| GET | `/api/users` | 人员列表 (分页+搜索) |
| DELETE | `/api/users/{id}` | 删除人员 |
| POST | `/api/users/{id}/sync` | 手动同步到 Pi |
| GET | `/api/pi/status` | Pi 在线检测 |
| GET/PUT | `/api/config` | 运行时配置读写 |
| GET | `/api/camera/status` | 摄像头状态 (含 stream_url) |

## Pi 同步

同步使用 HTTP API（非 SSH），流程：
1. 读取用户照片
2. POST 到 Pi `:5000/api/register` (multipart)
3. 成功 → 标记 `pi_synced=true`
4. 失败 → 创建 SyncTask 持久化任务，后台 worker 指数退避重试 (2s/5s/10s)
5. 进程重启后 SyncTask 队列不丢失 (SQLite)

## 运行时配置

`config.json` 字段：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| pi_host | 192.168.1.2 | Pi IP 地址 |
| pi_stream_port | 8080 | MJPEG 视频流端口 |
| pi_api_port | 5000 | 注册 API 端口 |
| pi_user | qxc | Pi 用户名 |
| pi_password | root | Pi 密码 |
| server_port | 8081 | Windows 后台端口 |
