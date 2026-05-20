# Windows 后台 — 理想架构文档

> 版本：v1.0 | 最后更新：2026-05-20 | 作者：向治昌

---

## 一、四端系统拓扑

```
                        ┌─────────────────────┐
       HTTP/HTTPS       │   Windows 后台 EXE   │
  ┌────────────────────►│                     │
  │  照片 + 姓名         │  FastAPI :8000       │
  │                     │  SQLite              │
  │  ┌──────────────────│  Vue 3 管理面板       │
  │  │                  └──────────┬──────────┘
  │  │                             │
  │  │ GET /api/users              │ HTTP (MJPEG + heartbeat)
  │  │ GET /api/camera              │ SSH/SCP (特征下发)
  │  │                              │
  │  ▼                              ▼
┌─┴────────┐              ┌─────────────────────┐
│ 手机小程序 │              │    树莓派 4B          │
│          │              │                     │
│ 拍照→上传 │              │ 摄像头采集 → AI 推理   │
│ 结果展示  │              │ FeatureDB 特征库      │
└──────────┘              │ UART → STM32         │
                          └──────────┬──────────┘
                                     │ UART 115200
                                     ▼
                          ┌─────────────────────┐
                          │   STM32F103C8T6      │
                          │   FreeRTOS           │
                          │   帧协议解析 → 动作执行 │
                          │   LED/继电器/蜂鸣器    │
                          └─────────────────────┘
```

---

## 二、数据流全景

### 2.1 注册链路（Phone → Windows → Pi）

```
手机拍照 ──POST /api/register──► Windows
                                   │
                                   ├─ 1. 保存照片到 photos/{id}/original.jpg
                                   ├─ 2. 写入 SQLite (users 表)
                                   ├─ 3. 提取人脸特征 (调用 Pi 或本地推理)
                                   ├─ 4. 保存特征文件 photos/{id}/feature.bin
                                   ├─ 5. SCP 下发特征到 Pi: features/{id}.bin
                                   │
                                   ▼
                                 树莓派 FeatureDB 热加载
```

### 2.2 摄像头监控链路（Pi → Windows）

```
树莓派 ──MJPEG stream──►    Windows /camera 页面
         :8080/stream          实时显示 320×240 画面

树莓派 ──POST /api/camera/heartbeat──►  Windows
         {status, fps, uptime}          Dashboard 状态卡片
```

### 2.3 识别执行链路（Pi → STM32）

```
树莓派 ──UART 帧协议──►  STM32
  SCRFD检测→对齐→提取→比对    识别成功→亮绿灯→开门
  识别失败→亮红灯→报警
```

### 2.4 管理链路（Browser → Windows）

```
管理员浏览器 ──GET /──►  Vue 3 SPA
                 │        ├─ Dashboard (统计概览)
                 │        ├─ /camera  (实时画面)
                 │        └─ /users   (人员管理)
                 │
                 └─ API 调用 (axios)
```

---

## 三、Windows 后台模块架构

```
Windows/
│
├── main.py                     # FastAPI 入口
│   ├── 挂载静态文件 (Vue dist/)
│   ├── 注册 API 路由
│   ├── CORS 中间件
│   └── 启动时自动打开浏览器
│
├── models.py                   # SQLAlchemy ORM 模型
│   ├── User(id, name, photo_path, feature_path, created_at)
│   ├── RecognitionLog(id, user_id, confidence, timestamp)
│   └── SystemStatus(id, pi_online, pi_uptime, camera_fps, updated_at)
│
├── config.py                   # 全局配置
│   ├── DATABASE_URL = sqlite:///face_recognition.db
│   ├── PHOTOS_DIR = ./photos/
│   ├── PI_HOST = 192.168.1.100
│   ├── PI_STREAM_URL = http://192.168.1.100:8080/stream
│   └── SERVER_PORT = 8000
│
├── database.py                 # 数据库会话管理
│   ├── init_db()               # 建表
│   └── get_session()           # 依赖注入
│
├── routes/
│   ├── register.py             # POST /api/register
│   ├── users.py                # GET/DELETE /api/users
│   ├── camera.py               # GET /api/camera/status, POST /api/camera/heartbeat
│   └── dashboard.py            # GET /api/dashboard
│
├── services/
│   ├── face_engine.py          # 人脸特征提取 (封装 verify.py 的 detect/extract)
│   └── pi_sync.py              # Pi 通信 (SCP 下发、心跳检测)
│
├── requirements.txt
├── build_exe.bat
│
├── frontend/                   # Vue 3 + Vite 项目
│   ├── src/
│   │   ├── App.vue             # 根布局 (侧栏 + router-view)
│   │   ├── main.ts             # 入口，注册插件
│   │   ├── style.css           # 暗色主题全局样式
│   │   ├── router/index.ts     # 路由 (/, /camera, /users)
│   │   ├── api/index.ts        # axios 封装
│   │   ├── components/
│   │   │   └── AppSidebar.vue  # 可折叠侧栏
│   │   └── views/
│   │       ├── Dashboard.vue   # 仪表盘
│   │       ├── Camera.vue      # 实时画面
│   │       └── Users.vue       # 人员管理
│   └── vite.config.ts
│
└── doc/
    ├── tasks.md                # 实现方案
    ├── architecture.md         # 本文档
    └── devlog.md               # 开发日志
```

---

## 四、数据库设计

### 4.1 users 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PK AUTOINCREMENT | 用户 ID |
| name | TEXT NOT NULL | 姓名 |
| photo_path | TEXT | 原始照片路径 |
| feature_path | TEXT | 特征文件路径 (.bin) |
| pi_synced | BOOLEAN DEFAULT 0 | 是否已同步到 Pi |
| created_at | DATETIME DEFAULT CURRENT_TIMESTAMP | 注册时间 |

### 4.2 recognition_logs 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PK AUTOINCREMENT | 日志 ID |
| user_id | INTEGER FK→users.id (nullable) | 识别到的用户 |
| name | TEXT | 识别结果姓名 |
| confidence | REAL | 置信度 |
| is_stranger | BOOLEAN DEFAULT 0 | 是否陌生人 |
| timestamp | DATETIME DEFAULT CURRENT_TIMESTAMP | 识别时间 |

### 4.3 system_status 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PK | 始终=1（单行） |
| pi_online | BOOLEAN DEFAULT 0 | 树莓派是否在线 |
| pi_uptime | INTEGER | Pi 运行秒数 |
| camera_fps | REAL | 当前推理帧率 |
| last_heartbeat | DATETIME | 上次心跳时间 |
| updated_at | DATETIME | 记录更新时间 |

---

## 五、API 接口契约

### 5.1 注册接口（手机端调用）

```
POST /api/register
Content-Type: multipart/form-data

参数:
  photo: File (jpg/png, ≤ 5MB)
  name:  string (1-20 字符)

响应 200:
{
  "id": 1,
  "name": "张三",
  "photo_url": "/photos/1/original.jpg",
  "created_at": "2026-05-20T21:00:00"
}

响应 400:
{ "detail": "姓名不能为空" }

响应 413:
{ "detail": "照片过大，最大 5MB" }
```

### 5.2 人员查询接口

```
GET /api/users?page=1&page_size=12&search=张

响应 200:
{
  "total": 15,
  "page": 1,
  "page_size": 12,
  "items": [
    {
      "id": 1,
      "name": "张三",
      "photo_url": "/photos/1/original.jpg",
      "created_at": "2026-05-20T21:00:00",
      "pi_synced": true
    }
  ]
}
```

### 5.3 删除用户

```
DELETE /api/users/{id}

响应 200: { "detail": "已删除" }
响应 404: { "detail": "用户不存在" }
```

### 5.4 摄像头状态

```
GET /api/camera/status

响应 200:
{
  "pi_online": true,
  "stream_url": "http://192.168.1.100:8080/stream",
  "camera_fps": 15.2,
  "pi_uptime": 86400,
  "last_heartbeat": "2026-05-20T20:59:30"
}
```

### 5.5 Pi 心跳上报

```
POST /api/camera/heartbeat
Content-Type: application/json

{
  "status": "online",
  "fps": 15.2,
  "uptime": 86400
}

响应 200: { "ack": true }
```

### 5.6 仪表盘统计

```
GET /api/dashboard

响应 200:
{
  "user_count": 12,
  "pi_online": true,
  "camera_fps": 15.2,
  "recent_users": [
    { "id": 12, "name": "王五", "created_at": "2分钟前" }
  ],
  "today_logs": 156
}
```

---

## 六、前端组件树与数据流

```
App.vue
├── AppSidebar.vue
│   └── navItems: [{path, icon, label}] ← router
│   └── collapsed: boolean ← 本地状态
│
└── <router-view>
    ├── Dashboard.vue
    │   ├── GET /api/dashboard ← onMounted
    │   ├── CountUp (数字滚动)
    │   └── v-motion (入场动效)
    │
    ├── Camera.vue
    │   ├── GET /api/camera/status ← 每 5 秒轮询
    │   ├── <img :src="streamUrl"> ← MJPEG 实时流
    │   └── pulse-ring 动画 (连接/断开)
    │
    └── Users.vue
        ├── GET /api/users?page=&search= ← 列表加载
        ├── DELETE /api/users/{id} ← 删除确认
        └── el-drawer (详情抽屉)
```

---

## 七、部署与打包架构

```
开发阶段:
  npm run dev (Vite :5173) ──proxy──► FastAPI :8000

生产打包:
  npm run build → frontend/dist/
  PyInstaller:
    --onefile main.py
    --add-data frontend/dist;frontend/dist
    → FaceRecognition.exe

运行时:
  双击 FaceRecognition.exe
  → 启动 FastAPI (uvicorn :8000)
  → 挂载 frontend/dist/ 为静态文件
  → 自动打开浏览器 http://localhost:8000
```

---

## 八、安全考量

| 层 | 措施 |
|----|------|
| 手机→Windows | 局域网部署，可选 HTTPS + Token 认证 |
| 文件上传 | 限制 5MB，校验 MIME type (image/jpeg, image/png) |
| CORS | 仅允许 localhost 和局域网 IP |
| SQLite | 仅本地访问，无网络暴露 |
| Pi→Windows | 心跳超时检测（>15s 判定离线） |
| 文件系统 | photos/ 目录不在静态文件挂载范围内，仅 API 返回 |

---

## 九、与 Office 项目的技术差异

| 维度 | Office 项目 | FaceRecognition 项目 |
|------|-----------|---------------------|
| 框架 | Electron 28 | FastAPI + PyInstaller |
| 前端 | Vite 5 (Electron 环境) | Vite 5 (浏览器 SPA) |
| 进程模型 | Main + Renderer + Preload | 单进程 (FastAPI) |
| IPC | contextBridge + ipcRenderer | HTTP REST + axios |
| 数据库 | sql.js (WASM) | SQLite + SQLAlchemy |
| 路由 | Vue Router (hash) | Vue Router (hash) |
| UI 风格 | Element Plus 亮/暗切换 | 自定义暗色玻璃拟态 |
| 部署 | Electron 打包 | PyInstaller 单文件 EXE |
```

---

## 十、未来扩展点

1. **本地人脸推理** — 将 verify.py 的 extract 函数集成进 Windows，注册时直接提取特征
2. **识别日志回传** — Pi 通过 HTTP 将识别结果回传到 Windows，存入 recognition_logs
3. **多摄像头支持** — 支持切换多个 Pi 的摄像头画面
4. **HTTPS + 证书** — 公网部署时加密传输
5. **WebSocket 推送** — Dashboard 实时更新（替代轮询）
6. **人员分组 + 权限** — 不同人脸对应不同开门权限
