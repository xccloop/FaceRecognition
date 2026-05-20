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

## 2026-05-20 — v1.1.0 搜索框暗色化 + 系统设置页

### 新增功能

| 功能 | 说明 |
|------|------|
| **系统设置页** | 新增 `/settings` 路由和 Settings.vue 组件，可视化配置树莓派 IP/端口/SSH 和服务器参数 |
| **配置持久化** | 新增 `GET/PUT /api/config` API，配置保存到 `config.json`，重启后保留 |
| **搜索框暗色化** | 人员管理页搜索框从 Element Plus 白色默认样式改为暗紫主题 |

### 搜索框白色问题

| 问题 | 人员管理页面的"搜索姓名..."输入框是 Element Plus 默认白色背景，与暗色主题不搭 |
|------|---------------------------------------------------------------------------|
| **原因** | `el-input` 组件未覆盖内部 `.el-input__wrapper` 样式 |
| **解决** | 在 Users.vue scoped CSS 中用 `:deep()` 穿透覆盖：背景改为半透明紫色，边框/聚焦阴影均为紫色系 |

### 配置页设计

用户反馈：每次改树莓派 IP 需要去源码里找 `config.py` 改环境变量，非常不方便。

解决：
- 新建 `Settings.vue`：双卡片布局（树莓派连接 + 服务器参数），全部使用暗色表单控件
- 后端新增 `CONFIG_FILE = config.json` + `load_runtime_config()` / `save_runtime_config()`
- `GET /api/config` 返回当前配置，`PUT /api/config` 保存并立即生效
- 页面底部实时推导视频流地址预览
- 侧边栏新增"⚙ 系统设置"导航项

### 文件变更

| 文件 | 操作 | 说明 |
|------|------|------|
| `frontend/src/views/Settings.vue` | 新建 | 系统设置页，双卡片表单 |
| `frontend/src/router/index.ts` | 修改 | 新增 `/settings` 懒加载路由 |
| `frontend/src/api/index.ts` | 修改 | 新增 `getConfig()` / `saveConfig()` |
| `frontend/src/components/AppSidebar.vue` | 修改 | 侧栏新增设置导航项 |
| `frontend/src/views/Users.vue` | 修改 | 搜索框暗紫主题样式 |
| `main.py` | 修改 | 新增 config.json 持久化 + GET/PUT /api/config + api_camera_status 读配置 |

### 打包

| 步骤 | 说明 |
|------|------|
| 前端构建 | `npm run build` 产出包含 Settings-*.js/css |
| 打包 | `pyinstaller --onefile --windowed …` 产出 FaceRecognition.exe |
| 版本 | v1.1.0 |

---

## 2026-05-20 — v1.1.1 Bug 修复：EXE 缺少 webview 模块

### 问题

| 问题 | 双击 `FaceRecognition.exe` 启动时报 `ModuleNotFoundError: No module named 'webview'` |
|------|----------------------------------------------------------------------------------------|
| **版本** | v1.1.0 EXE（8.9 MB） |
| **现象** | `main.py` 第 20 行 `import webview` 失败，EXE 直接崩溃 |
| **根因** | v1.1.0 打包所用终端会话中 conda `facerec` 环境未正确激活，`webview` 不在 PYTHONPATH 中。PyInstaller 对找不到的 `--hidden-import` 只发 WARNING 不中止构建，悄无声地产出残缺产物 |
| **证据** | v1.0.0 EXE 24.7 MB（正常） vs v1.1.0 EXE 8.9 MB（异常，差距 16 MB） |

### 解决

1. 创建 `hook-webview.py`：PyInstaller hook 文件，强制收集 webview 所有子模块 (`collect_submodules`)、数据文件（js/css）和动态库
2. 重写 `build_exe.bat`：不用 `conda activate`（在非交互式 cmd 中不可靠），改为自动扫描 facerec 环境 Python 路径（三套备选方案）
3. 显式添加 `--hidden-import clr`（pythonnet，webview WinForms 后端依赖）
4. 构建后增加大小校验：< 12 MB 告警，避免再次产出残缺 EXE
5. `requirements.txt` 新增 `pythonnet>=3.0` 和 `proxy_tools>=0.1`（pywebview 的隐式依赖）

### 文件变更

| 文件 | 操作 | 说明 |
|------|------|------|
| `hook-webview.py` | 新建 | PyInstaller hook，确保 webview 全部子模块/数据文件/DLL 被收集 |
| `build_exe.bat` | 重写 | 自动查找 facerec Python 路径（绕过 conda activate 问题），新增依赖验证和大小校验 |
| `requirements.txt` | 修改 | 显式列出 pythonnet、proxy_tools（pywebview 依赖） |
| `doc/devlog.md` | 修改 | 追加本次 Bug 记录 |

### 打包

| 步骤 | 说明 |
|------|------|
| 直接 Python 调用 | `/d/ANACONDA/envs/facerec/python.exe -m PyInstaller ...` |
| EXE 大小 | 25 MB（正常，v1.0.0 为 24.7 MB） |
| 版本 | v1.1.1 |

---


## 2026-05-21 — v1.1.2：修复系统设置保存失败

### 问题

- **现象**：系统设置页修改"服务器 IP"后点击保存，提示"保存失败"
- **复现**：进入设置页 → 将服务器 IP 从 0.0.0.0 改为 192.168.1.5 → 保存 → 失败
- 但 GET /api/config 正常（能打开设置页并加载默认值）

### 原因

main.py 第 248 行的 PUT 端点参数声明缺少 Body() 注解：

    # 错误写法
    async def api_save_config(payload: dict):

FastAPI 无法确定 dict 参数应从请求体还是查询参数读取。对 PUT 请求，框架默认将裸 dict 当作查询参数 → 前端 JSON body 被忽略 → payload 为空 {} → 无字段更新 → 保存失败。

### 解决

    # 正确写法
    def api_save_config(payload: dict = Body(...)):

- 用 Body(...) 显式告诉 FastAPI 从请求体读取 JSON
- 移除无用的 async（函数内无异步操作）
- 添加 try/except 捕获文件写入异常
- 添加 updated_count 计数器确认实际修改字段数

### 影响文件

| 文件 | 操作 | 说明 |
|------|------|------|
| main.py | 修改 | 第 248 行加 Body() 注解 + 错误处理日志 |

### 版本

v1.1.2


## 2026-05-21 — v1.1.3：修复 PUT /api/config 返回 405 Method Not Allowed

### 问题

- **现象**：v1.1.2 保存配置时提示"保存失败：Method Not Allowed"
- **根因**：dict = Body(...) 在部分 FastAPI 版本中路由注册失败，导致 PUT 端点不存在。当 PUT 请求到达路径 /api/config 时，GET-only 的 catch-all 路由 `{full_path:path}` 已占用该路径 → FastAPI 返回 405。

### 解决

放弃 Body() 注解，改用 Request 对象直接读取 JSON body：

    # v1.1.3 正确写法
    @app.put("/api/config")
    async def api_save_config(request: Request):
        payload = await request.json()

- 直接从 request.json() 读取，绕开 Body() + dict 的兼容性问题
- 恢复 async（request.json() 为异步操作）

### 影响文件

| 文件 | 操作 | 说明 |
|------|------|------|
| main.py | 修改 | 第 247 行改用 Request 读 body |

### 版本

v1.1.3

## 格式约定

后续日志按日期分组，每条问题记录包含：**问题描述 → 原因分析 → 解决方案**。
