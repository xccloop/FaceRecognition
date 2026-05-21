<template>
  <nav class="sidebar" :class="{ collapsed }">
    <!-- Logo -->
    <div class="logo" @click="$emit('toggle')">
      <span class="logo-icon">◈</span>
      <span v-show="!collapsed" class="logo-text">FaceID</span>
    </div>

    <!-- 导航项 -->
    <div class="nav-items">
      <router-link
        v-for="item in navItems"
        :key="item.path"
        :to="item.path"
        class="nav-item"
        active-class="active"
      >
        <span class="nav-icon">{{ item.icon }}</span>
        <span v-show="!collapsed" class="nav-label">{{ item.label }}</span>
        <span class="nav-indicator"></span>
      </router-link>
    </div>

    <!-- 底部折叠按钮 -->
    <div class="sidebar-footer">
      <button class="collapse-btn" @click="$emit('toggle')">
        {{ collapsed ? '▶' : '◀' }}
      </button>
    </div>
  </nav>
</template>

<script setup lang="ts">
defineProps<{ collapsed: boolean }>();
defineEmits<{ toggle: [] }>();

const navItems = [
  { path: "/", icon: "◆", label: "仪表盘" },
  { path: "/camera", icon: "◉", label: "实时画面" },
  { path: "/users", icon: "👥", label: "人员管理" },
  { path: "/settings", icon: "⚙", label: "系统设置" },
];
</script>

<style scoped>
.sidebar {
  position: fixed;
  left: 0;
  top: 0;
  bottom: 0;
  z-index: 100;
  width: var(--sidebar-width);
  background: rgba(15, 15, 26, 0.95);
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  transition: width 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
.sidebar.collapsed {
  width: var(--sidebar-collapsed);
}

.logo {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 24px 20px;
  cursor: pointer;
  user-select: none;
  overflow: hidden;
}
.logo-icon {
  font-size: 24px;
  color: var(--accent);
  flex-shrink: 0;
}
.logo-text {
  font-size: 18px;
  font-weight: 700;
  white-space: nowrap;
  background: linear-gradient(135deg, var(--accent), #a78bfa);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}

.nav-items {
  flex: 1;
  padding: 8px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.nav-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  border-radius: 10px;
  color: var(--text-secondary);
  text-decoration: none;
  position: relative;
  overflow: hidden;
  transition: all 0.2s ease;
  white-space: nowrap;
}
.nav-item:hover {
  background: rgba(124, 58, 237, 0.1);
  color: var(--text-primary);
}
.nav-item.active {
  background: rgba(124, 58, 237, 0.15);
  color: var(--accent);
}
.nav-icon {
  font-size: 20px;
  min-width: 24px;
  text-align: center;
  flex-shrink: 0;
}
.nav-label {
  opacity: 1;
  transition: opacity 0.2s;
}
.collapsed .nav-label {
  opacity: 0;
}
.nav-indicator {
  position: absolute;
  left: 0;
  top: 50%;
  transform: translateY(-50%);
  width: 3px;
  height: 0;
  background: var(--accent);
  border-radius: 0 3px 3px 0;
  transition: height 0.2s ease;
}
.nav-item.active .nav-indicator {
  height: 24px;
}

.sidebar-footer {
  padding: 12px;
  border-top: 1px solid var(--border);
}
.collapse-btn {
  width: 100%;
  padding: 8px;
  border: none;
  background: transparent;
  color: var(--text-secondary);
  cursor: pointer;
  border-radius: 8px;
  font-size: 14px;
  transition: all 0.2s;
}
.collapse-btn:hover {
  background: rgba(124, 58, 237, 0.1);
  color: var(--accent);
}
</style>
