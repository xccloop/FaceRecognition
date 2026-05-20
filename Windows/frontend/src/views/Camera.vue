<template>
  <div class="camera-page">
    <h2 v-motion-fade>实时画面</h2>
    <p class="camera-subtitle">树莓派摄像头 320×240 MJPEG 流</p>

    <div class="camera-wrapper">
      <div
        class="camera-frame"
        :class="{
          'pulse-green': cameraOnline,
          'pulse-red': !cameraOnline,
        }"
      >
        <img
          v-if="cameraOnline && streamUrl"
          :src="streamUrl"
          alt="摄像头实时画面"
          @error="onStreamError"
        />
        <div v-else class="no-signal">
          <span class="no-signal-icon">📷</span>
          <span class="no-signal-text">
            {{ cameraOnline ? '加载中...' : '等待树莓派摄像头连接' }}
          </span>
          <span class="no-signal-hint">
            Pi 上线后将自动显示实时画面
          </span>
        </div>
      </div>
    </div>

    <div class="camera-info">
      <span class="status-tag" :class="cameraOnline ? 'online' : 'offline'">
        <span class="dot"></span>
        {{ cameraOnline ? '已连接' : '未连接' }}
      </span>
      <span v-if="cameraOnline && cameraFps > 0">
        {{ cameraFps }} FPS · 320×240
      </span>
      <span v-else-if="!cameraOnline">
        目标: {{ configStreamUrl || '未配置' }}
      </span>
      <span v-else>
        已连接 · 等待画面
      </span>
      <span v-if="piUptime > 0" class="uptime">
        Pi 运行 {{ formatUptime(piUptime) }}
      </span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from "vue";
import { getCameraStatus } from "../api";

const cameraOnline = ref(false);
const streamUrl = ref("");
const cameraFps = ref(0);
const piUptime = ref(0);
const configStreamUrl = ref("");
let healthTimer: ReturnType<typeof setInterval>;

async function checkCamera() {
  try {
    const res = await getCameraStatus();
    const data = res.data;
    cameraOnline.value = data.pi_online;
    streamUrl.value = data.stream_url;
    cameraFps.value = data.camera_fps;
    piUptime.value = data.pi_uptime;
    if (!configStreamUrl.value) {
      configStreamUrl.value = data.stream_url || "";
    }
  } catch {
    cameraOnline.value = false;
  }
}

function onStreamError() {
  console.warn("MJPEG stream error, waiting for reconnect...");
  // 不立即设 offline，给一次重试机会
  setTimeout(() => {
    if (cameraOnline.value) {
      checkCamera();
    }
  }, 3000);
}

function formatUptime(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (h > 0) return `${h}小时${m}分钟`;
  return `${m}分钟`;
}

onMounted(() => {
  checkCamera();
  healthTimer = setInterval(checkCamera, 5000);
});

onUnmounted(() => {
  clearInterval(healthTimer);
});
</script>

<style scoped>
.camera-page {
  max-width: 500px;
  margin: 0 auto;
  text-align: center;
}
.camera-page h2 {
  margin-bottom: 4px;
  font-size: 22px;
}
.camera-subtitle {
  color: var(--text-secondary);
  font-size: 13px;
  margin-bottom: 24px;
}

.camera-wrapper {
  margin-bottom: 16px;
}

.camera-frame {
  width: 320px;
  height: 240px;
  margin: 0 auto;
  border-radius: 16px;
  overflow: hidden;
  background: #000;
  position: relative;
  border: 2px solid var(--border);
  transition: border-color 0.3s;
}
.camera-frame img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.pulse-green {
  animation: pulse-ring 2s infinite;
  border-color: var(--success);
}
.pulse-red {
  animation: pulse-ring-danger 2s infinite;
  border-color: var(--danger);
}

.no-signal {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  color: var(--text-secondary);
}
.no-signal-icon {
  font-size: 48px;
  opacity: 0.4;
}
.no-signal-text {
  font-size: 14px;
}
.no-signal-hint {
  font-size: 12px;
  opacity: 0.5;
}

.camera-info {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 16px;
  font-size: 13px;
  color: var(--text-secondary);
  flex-wrap: wrap;
}
.status-tag {
  display: flex;
  align-items: center;
  gap: 6px;
}
.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  display: inline-block;
}
.status-tag.online .dot {
  background: var(--success);
}
.status-tag.offline .dot {
  background: var(--danger);
}
.uptime {
  color: var(--accent);
}
</style>
