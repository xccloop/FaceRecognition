# Windows 后台 — 实现方案

> **第一身份：手机小程序的后台服务器。** 接收手机端上传的人脸照片和注册信息，存入数据库。
> **附加功能：管理面板。** 浏览器打开页面查看摄像头画面、管理已注册人员。
> 目标产物：单个 EXE，双击运行，自动打开浏览器。
> UI 风格：暗色主题 + 玻璃质感 + 灵动动画，与 Office 项目完全区分。

---

## 一、技术栈

| 层 | 选型 | 理由 |
|----|------|------|
| 后端 API | FastAPI (Python) | 轻量异步，自带文档 |
| 数据库 | SQLite + SQLAlchemy | 单文件零配置 |
| 前端 | Vue 3 + Vite | 组件化，热重载 |
| UI 框架 | Element Plus（按需引入） | 组件丰富，但主题要重新设计 |
| 路由 | Vue Router 4 | 多页面（摄像头 / 用户管理） |
| 动效 | CSS transitions + @vueuse/motion | 页面切换动画、卡片入场动效 |
| 打包 | PyInstaller | 单文件 EXE |
| 环境 | Conda (python=3.10) | 环境隔离 |

---

## 二、Phone ↔ Windows 数据流（核心）

```
手机小程序                          Windows 后台 EXE
─────────                          ────────────────
拍照 + 填姓名
    │
    │ POST /api/register
    │ {photo: jpg, name: "张三"}
    │ ─────────────────────────►  ┌─────────────────┐
    │                             │ 1. 保存照片到     │
    │                             │    photos/{id}/  │
    │                             │ 2. 写入 SQLite    │
    │                             │ 3. 返回 {id, name}│
    │ ◄─────────────────────────  └─────────────────┘
    │
  显示"注册成功"
    │
    │
管理员浏览器 ─────────────────────► 管理面板看到新人员
                                   （Dashboard 最近注册更新）
```

> 核心链路只有一条：手机拍照 → Windows 存照片 + 写库 → 管理面板展示。
> 所有人员数据都以 Windows 后台的 SQLite 为唯一数据源。

### Pi → Windows 摄像头实时流（第二条核心链路）

```
树莓派 4B                          Windows 后台 EXE
──────────                         ────────────────
USB/CSI 摄像头采集
    │
    │ MJPEG 推流
    │ http://pi:8080/stream
    │ ─────────────────────────►  管理面板 /camera 页面
    │                             实时显示 320×240 画面
    │                             连接状态：绿色脉冲光环
    │                             断开状态：红色闪烁警告
    │
    │ POST /api/camera/heartbeat
    │ {status: "online", fps: 15}  ← 树莓派定期上报状态
    │ ─────────────────────────►  Windows 记录 Pi 健康状态
    │                             Dashboard 显示「摄像头在线/离线」
```

> **这是系统的核心功能之一：管理员必须能在 Windows 端实时查看树莓派摄像头的画面，确认设备正常运行。** 页面 2（Camera）专门承载这个功能。Pi 未到位时显示占位状态，Pi 上线后自动切换为实时 MJPEG 流。


## 三、页面架构（3 页 + 侧栏导航）

```
┌──────┬────────────────────────────────────────┐
│      │                                        │
│  ◆    │           内容区（路由切换）             │
│ 仪表盘 │                                        │
│      │    页面1: 仪表盘（默认首页）               │
│  ◉    │    页面2: 实时画面                      │
│ 摄像头 │    页面3: 人员管理                      │
│      │                                        │
│  👥   │    （带页面切换过渡动画）                 │
│ 人员   │                                        │
│      │                                        │
└──────┴────────────────────────────────────────┘
```

侧栏可折叠，鼠标悬浮展开图标 + 文字。

---

## 四、各页面设计

### 页面 1：仪表盘（Dashboard）

不作为主页面，而是欢迎页/概览页，卡片式布局：

```
┌─────────────────────────────────────────────────────┐
│                                                     │
│         欢迎回来，管理员                              │
│         人脸识别门禁系统 v1.0                          │
│                                                     │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│   │ 👥       │  │ 📷       │  │ 🔵       │        │
│   │ 12       │  │ 在线     │  │ 正常     │        │
│   │ 注册人数  │  │ 摄像头   │  │ 系统状态  │        │
│   └──────────┘  └──────────┘  └──────────┘        │
│   ↑ 数字滚动动画  ↑ 动感圆点   ↑ 脉冲呼吸灯          │
│                                                     │
│   ┌─────────────────────────────────────────────┐  │
│   │  最近注册                                     │  │
│   │  ┌────┬──────┬──────────┐                   │  │
│   │  │头像│ 张三 │ 2分钟前   │  ← 从下往上滑入    │  │
│   │  │头像│ 李四 │ 15分钟前  │                   │  │
│   │  └────┴──────┴──────────┘                   │  │
│   └─────────────────────────────────────────────┘  │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### 页面 2：实时画面（Camera）

摄像头独占一页，居中展示，320×240：

```
┌─────────────────────────────────────────────────────┐
│                                                     │
│                                                     │
│           ┌─────────────────────┐                   │
│           │                     │                   │
│           │                     │                   │
│           │    320 × 240        │ ← 圆角边框        │
│           │                     │   连接时绿光脉冲   │
│           │                     │   断连时红光闪烁   │
│           └─────────────────────┘                   │
│                                                     │
│              ● 摄像头已连接                          │
│              1920×1080 → 320×240                    │
│                                                     │
│              [  切换摄像头  ]                        │
│                                                     │
└─────────────────────────────────────────────────────┘
```

画面容器带呼吸光效边框（连接时绿色，断开时红色）。画面下方显示连接状态 + 原始分辨率。

### 页面 3：人员管理（Users）

全功能管理页，搜索 + 卡片网格（不是呆板的表格）：

```
┌─────────────────────────────────────────────────────┐
│  人员管理                  🔍 搜索...  [ + 手动注册 ]│
│                                                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐         │
│  │          │  │          │  │          │         │
│  │   照片   │  │   照片   │  │   照片   │         │
│  │          │  │          │  │          │         │
│  │  张三    │  │  李四    │  │  王五    │         │
│  │  #001    │  │  #002    │  │  #003    │         │
│  │ 2分钟前  │  │ 15分钟前 │  │ 1小时前  │         │
│  └──────────┘  └──────────┘  └──────────┘         │
│  ↑ hover 时卡片上浮 + 阴影加深                     │
│                                                     │
│  点击卡片 → 弹出详情 Drawer（从右侧滑入）            │
│                                                     │
│         < 1   2   3   4   5 >                       │
│                                                     │
└─────────────────────────────────────────────────────┘
```

---

## 五、UI 风格规范

### 配色方案（暗色系，与 Office 项目的 Element Plus 默认蓝完全不同）

```
背景:    #0f0f1a (深紫黑)
卡片:    #1a1a2e (半透明深紫)  backdrop-filter: blur(12px)
文字:    #e0e0e0
强调色:  #7c3aed (紫色) → hover #8b5cf6
成功色:  #10b981 (翠绿)
危险色:  #ef4444 (红色)
边框:    rgba(124, 58, 237, 0.15)
```

### 动效规范

| 位置 | 效果 |
|------|------|
| 页面切换 | 淡入 + 轻微上移（fade + translateY 8px） |
| 卡片入场 | 交错延迟，逐个从下往上浮入 |
| 仪表盘数字 | 从 0 滚动到目标值（count-up 动画） |
| 摄像头边框 | 连接时绿色脉冲光环；断开时红色闪烁 |
| 人员卡片 hover | 上浮 4px + 阴影扩大 + 边框高亮 |
| 导航栏 | 图标 hover 微缩放 + 左侧紫色竖线滑入 |
| 侧栏折叠 | 图标间距平滑过渡，文字淡入淡出 |
| 抽屉弹窗 | 右侧滑入，带 backdrop 模糊 |
| 按钮点击 | 涟漪波纹效果（ripple） |

### 字体

使用系统字体栈，不引入额外 Web 字体：

```css
font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
```

---

## 六、环境搭建（Conda）

```bash
# 1. 创建 conda 环境
conda create -n facerec python=3.10 -y
conda activate facerec

# 2. 后端依赖
pip install fastapi uvicorn sqlalchemy aiofiles python-multipart

# 3. 打包工具
pip install pyinstaller

# 4. 前端（需要 Node.js 18+）
npm create vite@latest frontend -- --template vue-ts
cd frontend
npm install
npm install element-plus @element-plus/icons-vue vue-router@4 axios
npm install @vueuse/motion         # 动效库
npm install countup.js             # 数字滚动
```

---

## 七、项目结构

```
Windows/
├── main.py                     # FastAPI 入口 + 自动打开浏览器
├── models.py                   # SQLite 模型
├── config.py                   # 配置
├── photos/                     # 上传照片（自动创建）
├── face_recognition.db         # 数据库（自动创建）
├── requirements.txt
├── build_exe.bat               # 打包脚本
├── doc/
│   └── tasks.md
└── frontend/
    ├── package.json
    ├── vite.config.ts
    ├── index.html
    └── src/
        ├── App.vue             # 根组件（侧栏 + router-view）
        ├── main.ts             # 入口，注册 Element Plus + Router + Motion
        ├── style.css           # 全局暗色主题变量
        ├── router/
        │   └── index.ts        # 路由定义
        ├── api/
        │   └── index.ts        # axios 封装
        ├── components/
        │   └── AppSidebar.vue  # 侧栏导航组件
        └── views/
            ├── Dashboard.vue   # 仪表盘
            ├── Camera.vue      # 实时画面
            └── Users.vue       # 人员管理（卡片网格）
```

---

## 八、核心代码骨架

### 7.1 main.ts（前端入口 + 暗色主题变量）

```typescript
import { createApp } from 'vue';
import ElementPlus from 'element-plus';
import 'element-plus/dist/index.css';
import 'element-plus/theme-chalk/dark/css-vars.css';  // Element Plus 暗色变量
import { MotionPlugin } from '@vueuse/motion';
import App from './App.vue';
import router from './router';
import './style.css';  // 自定义暗色主题覆盖

const app = createApp(App);
app.use(ElementPlus);
app.use(router);
app.use(MotionPlugin);
app.mount('#app');
```

### 7.2 style.css（暗色主题）

```css
:root {
  --bg-primary: #0f0f1a;
  --bg-card: rgba(26, 26, 46, 0.85);
  --text-primary: #e0e0e0;
  --text-secondary: #a0a0b0;
  --accent: #7c3aed;
  --accent-hover: #8b5cf6;
  --success: #10b981;
  --danger: #ef4444;
  --border: rgba(124, 58, 237, 0.15);
  --sidebar-width: 220px;
  --sidebar-collapsed: 64px;
  color-scheme: dark;
}

* { margin: 0; padding: 0; box-sizing: border-box; }

body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  background: var(--bg-primary);
  color: var(--text-primary);
  overflow: hidden;
}

/* 页面切换动画 */
.page-enter-active { transition: opacity 0.3s ease, transform 0.3s ease; }
.page-leave-active { transition: opacity 0.2s ease, transform 0.2s ease; }
.page-enter-from { opacity: 0; transform: translateY(12px); }
.page-leave-to { opacity: 0; transform: translateY(-8px); }

/* 自定义滚动条 */
::-webkit-scrollbar { width: 6px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: rgba(124, 58, 237, 0.3); border-radius: 3px; }

/* 摄像头脉冲光环动画 */
@keyframes pulse-ring {
  0% { box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.5); }
  70% { box-shadow: 0 0 0 12px rgba(16, 185, 129, 0); }
  100% { box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
}
@keyframes pulse-ring-danger {
  0% { box-shadow: 0 0 0 0 rgba(239, 68, 68, 0.5); }
  70% { box-shadow: 0 0 0 12px rgba(239, 68, 68, 0); }
  100% { box-shadow: 0 0 0 0 rgba(239, 68, 68, 0); }
}

/* 卡片悬浮 */
.card-hover {
  transition: transform 0.25s cubic-bezier(0.4, 0, 0.2, 1),
              box-shadow 0.25s cubic-bezier(0.4, 0, 0.2, 1),
              border-color 0.25s ease;
}
.card-hover:hover {
  transform: translateY(-4px);
  box-shadow: 0 8px 30px rgba(124, 58, 237, 0.15);
  border-color: var(--accent);
}
```

### 7.3 App.vue（带侧栏的主布局）

```vue
<template>
  <div class="app-layout">
    <AppSidebar :collapsed="collapsed" @toggle="collapsed = !collapsed" />
    <main class="main-area" :class="{ expanded: collapsed }">
      <router-view v-slot="{ Component }">
        <transition name="page" mode="out-in">
          <component :is="Component" />
        </transition>
      </router-view>
    </main>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import AppSidebar from './components/AppSidebar.vue';
const collapsed = ref(false);
</script>

<style scoped>
.app-layout { display: flex; height: 100vh; }
.main-area {
  flex: 1; overflow-y: auto; padding: 32px;
  margin-left: var(--sidebar-width);
  transition: margin-left 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
.main-area.expanded { margin-left: var(--sidebar-collapsed); }
</style>
```

### 7.4 AppSidebar.vue（侧栏导航）

```vue
<template>
  <nav class="sidebar" :class="{ collapsed }">
    <div class="logo" @click="$emit('toggle')">
      <span class="logo-icon">◈</span>
      <span v-show="!collapsed" class="logo-text">FaceID</span>
    </div>

    <div class="nav-items">
      <router-link v-for="item in navItems" :key="item.path"
        :to="item.path" class="nav-item" active-class="active">
        <span class="nav-icon">{{ item.icon }}</span>
        <span v-show="!collapsed" class="nav-label">{{ item.label }}</span>
        <span class="nav-indicator"></span>
      </router-link>
    </div>

    <div class="sidebar-footer">
      <button class="collapse-btn" @click="$emit('toggle')">
        {{ collapsed ? '▶' : '◀' }}
      </button>
    </div>
  </nav>
</template>

<script setup lang="ts">
defineProps<{ collapsed: boolean }>();
defineEmits(['toggle']);

const navItems = [
  { path: '/', icon: '◆', label: '仪表盘' },
  { path: '/camera', icon: '◉', label: '实时画面' },
  { path: '/users', icon: '👥', label: '人员管理' },
];
</script>

<style scoped>
.sidebar {
  position: fixed; left: 0; top: 0; bottom: 0; z-index: 100;
  width: var(--sidebar-width);
  background: rgba(15, 15, 26, 0.95);
  backdrop-filter: blur(20px);
  border-right: 1px solid var(--border);
  display: flex; flex-direction: column;
  transition: width 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
.sidebar.collapsed { width: var(--sidebar-collapsed); }

.logo {
  display: flex; align-items: center; gap: 10px;
  padding: 24px 20px; cursor: pointer; user-select: none;
}
.logo-icon { font-size: 24px; color: var(--accent); }
.logo-text {
  font-size: 18px; font-weight: 700;
  background: linear-gradient(135deg, var(--accent), #a78bfa);
  -webkit-background-clip: text; -webkit-text-fill-color: transparent;
}

.nav-items { flex: 1; padding: 8px; display: flex; flex-direction: column; gap: 4px; }
.nav-item {
  display: flex; align-items: center; gap: 12px;
  padding: 12px 16px; border-radius: 10px;
  color: var(--text-secondary); text-decoration: none;
  position: relative; overflow: hidden;
  transition: all 0.2s ease;
}
.nav-item:hover { background: rgba(124, 58, 237, 0.1); color: var(--text-primary); }
.nav-item.active { background: rgba(124, 58, 237, 0.15); color: var(--accent); }
.nav-icon { font-size: 20px; min-width: 24px; text-align: center; }
.nav-label { white-space: nowrap; opacity: 1; transition: opacity 0.2s; }
.collapsed .nav-label { opacity: 0; }
.nav-indicator {
  position: absolute; left: 0; top: 50%; transform: translateY(-50%);
  width: 3px; height: 0; background: var(--accent); border-radius: 0 3px 3px 0;
  transition: height 0.2s ease;
}
.nav-item.active .nav-indicator { height: 24px; }

.sidebar-footer { padding: 12px; border-top: 1px solid var(--border); }
.collapse-btn {
  width: 100%; padding: 8px; border: none; background: transparent;
  color: var(--text-secondary); cursor: pointer; border-radius: 8px;
  transition: all 0.2s;
}
.collapse-btn:hover { background: rgba(124, 58, 237, 0.1); color: var(--accent); }
</style>
```

### 7.5 Dashboard.vue（仪表盘）

```vue
<template>
  <div class="dashboard">
    <div class="greeting">
      <h2>欢迎回来，管理员</h2>
      <p>人脸识别门禁系统 v1.0</p>
    </div>

    <div class="stat-grid">
      <div class="stat-card card-hover" v-motion-pop>
        <span class="stat-icon">👥</span>
        <span class="stat-value">
          <CountUp :endVal="stats.userCount" :duration="1.5" />
        </span>
        <span class="stat-label">注册人数</span>
      </div>
      <div class="stat-card card-hover" v-motion-pop>
        <span class="stat-icon">📷</span>
        <span class="stat-value status-dot" :class="stats.cameraOnline ? 'online' : 'offline'">
          {{ stats.cameraOnline ? '在线' : '离线' }}
        </span>
        <span class="stat-label">摄像头</span>
      </div>
      <div class="stat-card card-hover" v-motion-pop>
        <span class="stat-icon">🔵</span>
        <span class="stat-value status-dot online breathing">正常</span>
        <span class="stat-label">系统状态</span>
      </div>
    </div>

    <div class="recent-section" v-motion-slide-visible-bottom>
      <h3>最近注册</h3>
      <div class="recent-list">
        <div v-for="(u, i) in recentUsers" :key="u.id" class="recent-item"
             :style="{ animationDelay: i * 0.1 + 's' }">
          <el-avatar :src="u.photo_url" :size="40" shape="square" />
          <span class="recent-name">{{ u.name }}</span>
          <span class="recent-time">{{ u.created_at }}</span>
        </div>
        <el-empty v-if="!recentUsers.length" description="暂无注册记录" :image-size="80" />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue';
import CountUp from 'countup.js-vue';  // 或自己写一个简易版
import { getUsers } from '../api';

const stats = ref({ userCount: 0, cameraOnline: false });
const recentUsers = ref([]);

onMounted(async () => {
  const res = await getUsers(1, 10);
  stats.value.userCount = res.data.total;
  recentUsers.value = (res.data.items || []).slice(0, 5);
});
</script>

<style scoped>
.dashboard { max-width: 900px; }
.greeting { margin-bottom: 32px; }
.greeting h2 { font-size: 28px; font-weight: 700; margin-bottom: 6px; }
.greeting p { color: var(--text-secondary); font-size: 15px; }

.stat-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; margin-bottom: 32px; }
.stat-card {
  background: var(--bg-card); backdrop-filter: blur(12px);
  border: 1px solid var(--border); border-radius: 14px;
  padding: 24px; display: flex; flex-direction: column; gap: 6px;
}
.stat-icon { font-size: 28px; }
.stat-value { font-size: 32px; font-weight: 700; }
.stat-label { font-size: 13px; color: var(--text-secondary); }

.status-dot { position: relative; }
.status-dot.online { color: var(--success); }
.status-dot.offline { color: var(--danger); }
.breathing {
  animation: breathe 2s ease-in-out infinite;
}
@keyframes breathe {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.recent-section { background: var(--bg-card); border: 1px solid var(--border); border-radius: 14px; padding: 24px; }
.recent-section h3 { font-size: 16px; margin-bottom: 16px; }
.recent-item {
  display: flex; align-items: center; gap: 12px;
  padding: 10px 0; border-bottom: 1px solid var(--border);
  animation: slideUp 0.4s ease both;
}
.recent-item:last-child { border-bottom: none; }
.recent-name { flex: 1; font-weight: 500; }
.recent-time { color: var(--text-secondary); font-size: 13px; }

@keyframes slideUp {
  from { opacity: 0; transform: translateY(12px); }
  to { opacity: 1; transform: translateY(0); }
}
</style>
```

### 7.6 Camera.vue（实时画面）

```vue
<template>
  <div class="camera-page">
    <h2>实时画面</h2>
    <div class="camera-wrapper" :class="{ connected: cameraOnline, disconnected: !cameraOnline }">
      <div class="camera-frame" :class="{ 'pulse-green': cameraOnline, 'pulse-red': !cameraOnline }">
        <img v-if="cameraUrl" :src="cameraUrl" alt="摄像头画面" />
        <div v-else class="no-signal">
          <span class="no-signal-icon">📷</span>
          <span>等待摄像头连接</span>
        </div>
      </div>
    </div>
    <div class="camera-info">
      <span class="status-tag" :class="cameraOnline ? 'online' : 'offline'">
        <span class="dot"></span>
        {{ cameraOnline ? '已连接' : '未连接' }}
      </span>
      <span v-if="cameraOnline">1920×1080 → 320×240</span>
      <span v-else>目标: {{ streamUrl || '未配置' }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue';
import { getCameraUrl } from '../api';

const cameraUrl = ref('');
const cameraOnline = ref(false);
const streamUrl = ref('');
let healthTimer: number;

async function checkCamera() {
  try {
    const res = await getCameraUrl();
    streamUrl.value = res.data.url;
    cameraUrl.value = res.data.url;
    cameraOnline.value = true;
  } catch {
    cameraOnline.value = false;
  }
}

onMounted(() => {
  checkCamera();
  healthTimer = window.setInterval(checkCamera, 5000);
});
onUnmounted(() => clearInterval(healthTimer));
</script>

<style scoped>
.camera-page { max-width: 500px; margin: 0 auto; text-align: center; }
.camera-page h2 { margin-bottom: 24px; font-size: 22px; }

.camera-frame {
  width: 320px; height: 240px; margin: 0 auto;
  border-radius: 16px; overflow: hidden;
  background: #000; position: relative;
  border: 2px solid var(--border);
  transition: border-color 0.3s;
}
.camera-frame img { width: 100%; height: 100%; object-fit: contain; }

.pulse-green { animation: pulse-ring 2s infinite; border-color: var(--success); }
.pulse-red { animation: pulse-ring-danger 2s infinite; border-color: var(--danger); }

.no-signal {
  width: 100%; height: 100%; display: flex; flex-direction: column;
  align-items: center; justify-content: center; gap: 12px;
  color: var(--text-secondary);
}
.no-signal-icon { font-size: 48px; opacity: 0.4; }

.camera-info {
  margin-top: 20px; display: flex; align-items: center;
  justify-content: center; gap: 16px;
  font-size: 13px; color: var(--text-secondary);
}
.status-tag { display: flex; align-items: center; gap: 6px; }
.dot { width: 8px; height: 8px; border-radius: 50%; }
.status-tag.online .dot { background: var(--success); }
.status-tag.offline .dot { background: var(--danger); }
</style>
```

### 7.7 Users.vue（人员管理 - 卡片网格）

```vue
<template>
  <div class="users-page">
    <div class="page-header">
      <h2>人员管理</h2>
      <div class="header-actions">
        <el-input v-model="search" placeholder="搜索姓名..." prefix-icon="Search"
                  class="search-input" clearable @input="onSearch" />
      </div>
    </div>

    <div class="user-grid" v-loading="loading">
      <transition-group name="card-list">
        <div v-for="user in users" :key="user.id" class="user-card card-hover"
             @click="viewUser(user)" v-motion-pop>
          <div class="card-photo">
            <el-image :src="user.photo_url" fit="cover" />
          </div>
          <div class="card-info">
            <span class="card-name">{{ user.name }}</span>
            <span class="card-id">#{{ String(user.id).padStart(3, '0') }}</span>
            <span class="card-time">{{ formatTime(user.created_at) }}</span>
          </div>
          <el-button class="delete-btn" circle size="small" type="danger" :icon="Delete"
                     @click.stop="confirmDelete(user)" />
        </div>
      </transition-group>
    </div>

    <el-empty v-if="!loading && !users.length" description="暂无注册人员" />

    <div class="pagination" v-if="total > pageSize">
      <el-pagination v-model:current-page="page" :page-size="pageSize"
                     :total="total" background layout="prev, pager, next"
                     @current-change="loadUsers" />
    </div>

    <!-- 用户详情 Drawer -->
    <el-drawer v-model="drawerVisible" :title="detailUser?.name" direction="rtl" size="420px">
      <div class="user-detail" v-if="detailUser">
        <el-image :src="detailUser.photo_url" fit="contain"
                  style="width:100%; border-radius:12px; margin-bottom:20px" />
        <el-descriptions :column="1" border>
          <el-descriptions-item label="姓名">{{ detailUser.name }}</el-descriptions-item>
          <el-descriptions-item label="编号">#{{ String(detailUser.id).padStart(3, '0') }}</el-descriptions-item>
          <el-descriptions-item label="注册时间">{{ detailUser.created_at }}</el-descriptions-item>
        </el-descriptions>
        <el-button type="danger" style="margin-top:20px;width:100%"
                   @click="confirmDelete(detailUser)">删除此人</el-button>
      </div>
    </el-drawer>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue';
import { Delete } from '@element-plus/icons-vue';
import { ElMessage, ElMessageBox } from 'element-plus';
import { getUsers, deleteUser } from '../api';

const users = ref([]);
const total = ref(0);
const page = ref(1);
const pageSize = ref(12);
const loading = ref(false);
const search = ref('');
const drawerVisible = ref(false);
const detailUser = ref(null);
let timer: number;

async function loadUsers() {
  loading.value = true;
  try {
    const res = await getUsers(page.value, pageSize.value, search.value);
    users.value = res.data.items;
    total.value = res.data.total;
  } finally {
    loading.value = false;
  }
}

function onSearch() {
  clearTimeout(timer);
  timer = window.setTimeout(() => { page.value = 1; loadUsers(); }, 300);
}

async function viewUser(user: any) {
  detailUser.value = user;
  drawerVisible.value = true;
}

async function confirmDelete(user: any) {
  try {
    await ElMessageBox.confirm(`确认删除「${user.name}」？`, '删除确认', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消'
    });
    await deleteUser(user.id);
    ElMessage.success(`已删除 ${user.name}`);
    drawerVisible.value = false;
    loadUsers();
  } catch {}
}

function formatTime(t: string) {
  if (!t) return '';
  const diff = Date.now() - new Date(t).getTime();
  const mins = Math.floor(diff / 60000);
  if (mins < 1) return '刚刚';
  if (mins < 60) return `${mins}分钟前`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}小时前`;
  return `${Math.floor(hours / 24)}天前`;
}

onMounted(loadUsers);
</script>

<style scoped>
.users-page { max-width: 1000px; }
.page-header {
  display: flex; align-items: center; justify-content: space-between;
  margin-bottom: 24px;
}
.page-header h2 { font-size: 22px; }
.search-input { width: 240px; }

.user-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 16px;
}

.user-card {
  background: var(--bg-card); backdrop-filter: blur(12px);
  border: 1px solid var(--border); border-radius: 14px;
  overflow: hidden; position: relative; cursor: pointer;
}
.card-photo { width: 100%; height: 180px; background: #000; overflow: hidden; }
.card-photo img { width: 100%; height: 100%; object-fit: cover; transition: transform 0.3s; }
.user-card:hover .card-photo img { transform: scale(1.05); }
.card-info { padding: 14px; }
.card-name { font-size: 15px; font-weight: 600; display: block; }
.card-id { font-size: 12px; color: var(--accent); }
.card-time { font-size: 11px; color: var(--text-secondary); display: block; margin-top: 4px; }
.delete-btn {
  position: absolute; top: 8px; right: 8px;
  opacity: 0; transition: opacity 0.2s;
}
.user-card:hover .delete-btn { opacity: 1; }

.pagination { display: flex; justify-content: center; margin-top: 32px; }

.card-list-enter-active { transition: all 0.4s ease; }
.card-list-leave-active { transition: all 0.3s ease; }
.card-list-enter-from { opacity: 0; transform: translateY(20px); }
.card-list-leave-to { opacity: 0; transform: scale(0.9); }
</style>
```

---

## 九、路由配置 router/index.ts

```typescript
import { createRouter, createWebHashHistory } from 'vue-router';

const routes = [
  { path: '/', name: 'Dashboard', component: () => import('../views/Dashboard.vue') },
  { path: '/camera', name: 'Camera', component: () => import('../views/Camera.vue') },
  { path: '/users', name: 'Users', component: () => import('../views/Users.vue') },
];

export default createRouter({
  history: createWebHashHistory(),  // Hash 模式，PyInstaller 打包后兼容 file://
  routes,
});
```

---

## 十、打包为 EXE

```bat
@echo off
REM build_exe.bat

cd frontend
call npm run build
cd ..

pyinstaller ^
  --name FaceRecognition ^
  --onefile ^
  --console ^
  --add-data "frontend/dist;frontend/dist" ^
  --hidden-import fastapi ^
  --hidden-import uvicorn.loops.auto ^
  --hidden-import uvicorn.protocols.http.auto ^
  --hidden-import sqlalchemy ^
  --hidden-import aiofiles ^
  main.py

echo 完成: dist\FaceRecognition.exe
pause
```

---

## 十一、开发步骤

```
第 1 步：Conda 环境 + 后端 API
  conda create -n facerec python=3.10 -y && conda activate facerec
  pip install fastapi uvicorn sqlalchemy aiofiles python-multipart
  → 写 main.py / models.py / config.py
  → python main.py 启动验证

第 2 步：前端脚手架 + 暗色主题
  npm create vite@latest frontend -- --template vue-ts
  cd frontend && npm install && npm install element-plus @element-plus/icons-vue vue-router@4 axios @vueuse/motion
  → 配 vite.config.ts（开发代理到 localhost:8000）
  → 写全局暗色 style.css

第 3 步：实现三个页面
  → AppSidebar.vue（侧栏 + 折叠动效）
  → Dashboard.vue（统计卡片 + 最近注册）
  → Camera.vue（320×240 + 脉冲光环边框）
  → Users.vue（卡片网格 + hover 上浮 + 右侧抽屉）

第 4 步：前后端联通
  → npm run build
  → 重启 python main.py
  → 浏览器打开 http://localhost:8000 验证所有页面

第 5 步：手机上传测试
  → curl / Postman 模拟 POST /api/register
  → 确认照片出现在 Users 页

第 6 步：打包 EXE
  → 运行 build_exe.bat
  → dist/FaceRecognition.exe 双击测试
```

---

## 十二、与 Office UI 的区分对照

| 特征 | Office 项目 | FaceRecognition |
|------|------------|----------------|
| 主题 | Element Plus 默认亮/暗切换 | 自定义暗色主题（深紫黑底色） |
| 强调色 | #409EFF (蓝色) | #7c3aed (紫色) |
| 布局 | 顶部导航 + 内容区 | 侧栏导航（可折叠） |
| 页面切换 | 无动画 | fade + translateY 过渡 |
| 用户列表 | 表格 | 卡片网格 + hover 上浮动效 |
| 摄像头 | 无 | 320×240 圆角框 + 脉冲光环 |
| 弹窗 | Dialog 居中 | Drawer 右侧滑入 |
| 仪表盘 | 无 | 统计卡片 + 数字滚动 + 入场动效 |
| 质感 | 标准 Material | 玻璃拟态 backdrop-filter blur |
