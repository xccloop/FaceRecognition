# Windows 后台 — 开发日志

> 记录开发过程中遇到的问题、根本原因和解决方案，供后续参考。

---

## 2026-05-20 — 项目初始化

### 环境准备

- **开发机**: Windows 11 + Git Bash
- **Python**: Conda 环境 `facerec` (python=3.10)
- **Node.js**: 18+
- **目标**: 从零构建 Windows 后台，产出单文件 EXE

### 文件编辑工具选择

| 问题 | 在 Windows (CRLF) 环境下，patch 工具反复报 "Post-write verification failed" |
|------|---------------------------------------------------------------------------|
| **原因** | Git Bash 环境下的文件默认 CRLF 行尾，patch 工具写入 LF 后读回 CRLF，校验长度不一致 |
| **解决** | 对于 `.md` 等文档文件，使用 `sed -i` 直接编辑；`.py` / `.ts` / `.vue` 等代码文件通过 write_file 工具创建（该工具正确处理了 CRLF） |
| **影响范围** | tasks.md 的增量修改；所有新建文件不受影响 |

### 文档先行原则

在开始编码前，先完成以下文档：

1. `doc/tasks.md` — 实现方案（已在之前会话完成）
2. `doc/architecture.md` — 理想架构文档（本次新建）
3. `doc/devlog.md` — 开发日志（本文档）

---

## 2026-05-20 — 后端构建

### 技术选型确认

| 组件 | 选型 | 理由 |
|------|------|------|
| Web 框架 | FastAPI | 异步、自带文档、类型安全 |
| ORM | SQLAlchemy 2.0 | 成熟稳定、声明式模型 |
| 数据库 | SQLite | 单文件零配置 |
| 静态文件 | FastAPI StaticFiles + FileResponse | 挂载 Vue dist/，SPA fallback |
| 图片存储 | 本地文件系统 | 简单可靠 |
| 打包 | PyInstaller --onefile | 单 EXE 交付 |

### 前后端分离开发模式

开发时 Vite dev server (:5173) 通过 proxy 转发 API 到 FastAPI (:8000)。
生产时 FastAPI 直接 serve Vue 构建产物。

```typescript
// vite.config.ts — 开发代理
server: {
  proxy: {
    '/api': 'http://localhost:8000',
    '/photos': 'http://localhost:8000',
  }
}
```

---

## 2026-05-20 — 前端构建

### Element Plus 暗色主题适配

| 问题 | Element Plus 默认主题与项目暗色玻璃风格冲突 |
|------|------------------------------------------|
| **解决** | 引入 `element-plus/theme-chalk/dark/css-vars.css` 开启官方暗色变量，再用自定义 `style.css` 覆盖强调色和背景色 |

### Vue 页面切换动画

| 问题 | 页面切换生硬 |
|------|------------|
| **解决** | 使用 `<transition name="page" mode="out-in">` + CSS 定义 `.page-enter-from` / `.page-leave-to` 实现淡入上移动画 |

---

## 2026-05-20 — 实际构建问题与解决

### 1. vue-tsc 与 TypeScript 5.8+ 不兼容

| 问题 | vue-tsc 报 "Search string not found" 异常 |
|------|-------------------------------------------|
| **原因** | vue-tsc@1.8 不支持 TypeScript 5.8 的内部 API 变更 |
| **解决** | 修改 package.json build 脚本为 `vite build`（跳过类型检查），类型检查留给 IDE 和 CI |

### 2. @vueuse/motion 缺少 defu 依赖

| 问题 | Vite build 报 Rollup failed to resolve import "defu" |
|------|--------------------------------------------------------|
| **原因** | @vueuse/motion@2.1.0 的 peerDependencies 声明了 defu 但 npm 10 不会自动安装 peer 依赖 |
| **解决** | `npm install defu` 手动安装 |

### 3. countup.js-vue 不存在于 npm

| 问题 | import CountUp from "countup.js-vue" 构建失败 (E404) |
|------|--------------------------------------------------------|
| **原因** | `countup.js-vue` 不是官方发布的 npm 包名 |
| **解决** | Dashboard 直接显示数字值 `<span class="stat-highlight">{{ card.value }}</span>`，去掉 CountUp 依赖。数字滚动效果待后续自实现或用其他库 |

### 4. Python SyntaxError: global before reference

| 问题 | main.py 中 `global BASE_DIR` 在 `if __name__ == "__main__"` block 内，但这些变量已在模块顶层引用（config 行） |
|------|----------------------------------------------------------------------------------------------------------|
| **原因** | Python 禁止在作用域中用 `global` 声明一个已经在此作用域之前被引用的名称 |
| **解决** | 重写 main.py：移除 global 声明，改为 `import main as _main; _main.PHOTOS_DIR = new_photos` 直接覆盖模块级变量 |

### 5. CRLF 行尾与 patch 工具冲突

| 问题 | patch 工具反复报 "Post-write verification failed" |
|------|---------------------------------------------------|
| **原因** | Windows Git Bash 环境下文件默认 CRLF (`\r\n`) 行尾。patch 工具写入 LF 后读回 CRLF 校验长度不一致 |
| **解决** | 使用 `sed -i` 直接编辑（如 package.json），或使用 write_file 工具创建新文件（该工具自动处理 CRLF） |

---

## 2026-05-20 — 前后端联通验证

| 测试项 | 状态 | 验证方式 |
|--------|------|----------|
| GET /api/dashboard | ✅ 通过 | curl 返回 user_count=0, pi_online=false |
| GET /api/users | ✅ 通过 | curl 返回分页空列表 |
| GET /api/camera/status | ✅ 通过 | curl 返回 pi_online=false, stream_url="" |
| GET / (前端SPA) | ✅ 通过 | HTTP 200，返回 index.html |
| POST /api/register | ⏳ 未测 | 需模拟文件上传 |
| DELETE /api/users/{id} | ⏳ 未测 | 需先注册用户 |

---

## 2026-05-20 — 打包为 EXE

### PyInstaller 配置要点

| 项目 | 配置 |
|------|------|
| 命令 | `pyinstaller --onefile --console --name FaceRecognition main.py` |
| 数据文件 | `--add-data "frontend/dist;frontend/dist" --add-data "photos;photos"` |
| hidden-import | sqlalchemy, uvicorn, fastapi, aiofiles, python_multipart, uvicorn.loops.auto, uvicorn.protocols.http.auto 等 |
| collect-all | starlette, fastapi |

详见 `build_exe.bat`。

### 静态文件路径 Fix

| 问题 | 打包后 Vue 前端 404 |
|------|-------------------|
| **原因** | PyInstaller 打包后 `__file__` 指向临时解压目录，需用 `sys._MEIPASS` 获取资源路径 |
| **解决** | 在 `main.py` 的 `if __name__ == "__main__"` 中判断 `getattr(sys, 'frozen', False)` 来决定数据文件路径，通过 `import main as _main` 覆盖模块级变量 |

---

## 待解决问题

### Pi 摄像头 MJPEG 流

| 状态 | BLOCKED — 等树莓派硬件到位 |
|------|--------------------------|
| **当前行为** | Camera 页面显示"等待摄像头连接"占位 |
| **预期行为** | Pi 上线后自动显示实时 MJPEG 流 |

### 人脸特征提取

| 状态 | 待实现 |
|------|--------|
| **方案** | 复用项目已有的 `verify.py` 中的 detect/extract 函数 |
| **依赖** | opencv-python, insightface, onnxruntime |

### 树莓派 SCP 下发

| 状态 | 待实现 |
|------|--------|
| **方案** | paramiko 库 SSH/SCP |
| **依赖** | Pi 上线、网络互通 |

---

## 格式约定

后续日志按日期分组，每条问题记录包含：**问题描述 → 原因分析 → 解决方案**。
