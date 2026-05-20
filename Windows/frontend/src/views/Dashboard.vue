<template>
  <div class="dashboard">
    <!-- 欢迎 -->
    <div class="greeting" v-motion-fade>
      <h2>欢迎回来，管理员</h2>
      <p>人脸识别门禁系统 v1.0</p>
    </div>

    <!-- 统计卡片 -->
    <div class="stat-grid">
      <div
        class="stat-card card-hover"
        v-motion-pop
        v-for="(card, idx) in statCards"
        :key="idx"
        :style="{ transitionDelay: idx * 0.08 + 's' }"
      >
        <span class="stat-icon">{{ card.icon }}</span>
        <span class="stat-value" :class="card.valueClass">
          <template v-if="card.isNumber">
            {{ card.value }}
          </template>
          <template v-else>
            <span class="status-dot" :class="card.statusClass">
              {{ card.value }}
            </span>
          </template>
        </span>
        <span class="stat-label">{{ card.label }}</span>
      </div>
    </div>

    <!-- 最近注册 -->
    <div class="recent-section" v-motion-slide-visible-bottom>
      <h3>最近注册</h3>
      <div class="recent-list" v-if="recentUsers.length">
        <div
          v-for="(u, i) in recentUsers"
          :key="u.id"
          class="recent-item"
          :style="{ animationDelay: i * 0.1 + 's' }"
        >
          <el-avatar :src="u.photo_url || undefined" :size="40" shape="square">
            {{ u.name.charAt(0) }}
          </el-avatar>
          <span class="recent-name">{{ u.name }}</span>
          <span class="recent-time">{{ u.created_at }}</span>
        </div>
      </div>
      <el-empty
        v-else
        description="暂无注册记录"
        :image-size="80"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from "vue";
import { getDashboard } from "../api";


interface RecentUser {
  id: number;
  name: string;
  photo_url: string;
  created_at: string;
}

const userCount = ref(0);
const piOnline = ref(false);
const cameraFps = ref(0);
const recentUsers = ref<RecentUser[]>([]);

const statCards = computed(() => [
  {
    icon: "👥",
    value: userCount.value,
    label: "注册人数",
    isNumber: true,
    valueClass: "stat-highlight",
  },
  {
    icon: "📷",
    value: piOnline.value ? "在线" : "离线",
    label: "摄像头",
    isNumber: false,
    statusClass: piOnline.value ? "online" : "offline",
    valueClass: "",
  },
  {
    icon: "🔵",
    value: "正常",
    label: "系统状态",
    isNumber: false,
    statusClass: "online breathing",
    valueClass: "",
  },
]);

onMounted(async () => {
  try {
    const res = await getDashboard();
    const data = res.data;
    userCount.value = data.user_count || 0;
    piOnline.value = data.pi_online || false;
    cameraFps.value = data.camera_fps || 0;
    recentUsers.value = data.recent_users || [];
  } catch (err) {
    console.error("Dashboard load failed:", err);
  }
});
</script>

<style scoped>
.dashboard {
  max-width: 900px;
}

.greeting {
  margin-bottom: 32px;
}
.greeting h2 {
  font-size: 28px;
  font-weight: 700;
  margin-bottom: 6px;
}
.greeting p {
  color: var(--text-secondary);
  font-size: 15px;
}

.stat-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 16px;
  margin-bottom: 32px;
}

.stat-card {
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 24px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.stat-icon {
  font-size: 28px;
}
.stat-value {
  font-size: 32px;
  font-weight: 700;
}
.stat-label {
  font-size: 13px;
  color: var(--text-secondary);
}

.status-dot {
  position: relative;
  display: inline-block;
}
.status-dot.online {
  color: var(--success);
}
.status-dot.offline {
  color: var(--danger);
}
.breathing {
  animation: breathe 2s ease-in-out infinite;
}

.recent-section {
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 24px;
}
.recent-section h3 {
  font-size: 16px;
  margin-bottom: 16px;
}
.recent-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 0;
  border-bottom: 1px solid var(--border);
  animation: slideUp 0.4s ease both;
}
.recent-item:last-child {
  border-bottom: none;
}
.recent-name {
  flex: 1;
  font-weight: 500;
}
.recent-time {
  color: var(--text-secondary);
  font-size: 13px;
}
</style>
