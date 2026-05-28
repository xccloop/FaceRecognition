<template>
  <div class="settings-page">
    <div class="page-header" v-motion-fade>
      <h2>系统设置</h2>
      <p class="page-desc">配置树莓派 HTTP API 连接、服务器参数等</p>
    </div>

    <div class="settings-cards">
      <!-- 树莓派配置 -->
      <div class="setting-card card-hover" v-motion-pop>
        <div class="card-icon">🍓</div>
        <h3>树莓派连接</h3>
        <el-form :model="form" label-position="top" class="setting-form">
          <el-form-item label="树莓派 IP 地址">
            <el-input
              v-model="form.pi_host"
              placeholder="192.168.1.100"
              class="dark-input"
            />
          </el-form-item>
          <el-form-item label="视频流端口 (MJPEG)">
            <el-input-number
              v-model="form.pi_stream_port"
              :min="1"
              :max="65535"
              class="dark-number"
            />
          </el-form-item>
          <el-form-item label="API 端口 (注册服务)">
            <el-input-number
              v-model="form.pi_api_port"
              :min="1"
              :max="65535"
              class="dark-number"
            />
            <span class="form-hint">Pi 端 pi_server.py 的端口，默认 5000</span>
          </el-form-item>
        </el-form>
      </div>

      <!-- 服务器配置 -->
      <div class="setting-card card-hover" v-motion-pop>
        <div class="card-icon">⚙️</div>
        <h3>服务器参数</h3>
        <el-form :model="form" label-position="top" class="setting-form">
          <el-form-item label="服务器端口">
            <el-input-number
              v-model="form.server_port"
              :min="80"
              :max="65535"
              class="dark-number"
            />
            <span class="form-hint">修改后需重启生效</span>
          </el-form-item>
          <el-form-item label="心跳超时 (秒)">
            <el-input-number
              v-model="form.heartbeat_timeout"
              :min="3"
              :max="120"
              class="dark-number"
            />
          </el-form-item>
          <el-form-item label="服务器 IP">
            <el-input
              v-model="form.server_host"
              placeholder="0.0.0.0"
              class="dark-input"
            />
            <span class="form-hint">修改后需重启生效</span>
          </el-form-item>
        </el-form>
      </div>
    </div>

    <!-- 预览：推导出的地址 -->
    <div class="preview-bar" v-motion-fade>
      <div class="preview-item">
        <span class="preview-label">视频流：</span>
        <code class="preview-url">{{ computedStreamUrl }}</code>
      </div>
      <div class="preview-item">
        <span class="preview-label">注册 API：</span>
        <code class="preview-url">{{ computedApiUrl }}</code>
      </div>
    </div>

    <!-- 操作按钮 -->
    <div class="actions" v-motion-fade>
      <el-button
        type="primary"
        size="large"
        :loading="saving"
        @click="saveConfig"
        class="save-btn"
      >
        保存配置
      </el-button>
      <el-button size="large" @click="resetForm" class="reset-btn">
        恢复默认
      </el-button>
    </div>

    <!-- 保存结果 -->
    <el-alert
      v-if="saveMsg"
      :title="saveMsg"
      :type="saveMsgType"
      show-icon
      closable
      style="margin-top: 16px"
    />
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from "vue";
import { ElMessage } from "element-plus";
import { getConfig, saveConfig as apiSaveConfig } from "../api";

interface ConfigForm {
  pi_host: string;
  pi_stream_port: number;
  pi_api_port: number;
  server_port: number;
  server_host: string;
  heartbeat_timeout: number;
}

const form = ref<ConfigForm>({
  pi_host: "192.168.1.100",
  pi_stream_port: 8080,
  pi_api_port: 5000,
  server_port: 8000,
  server_host: "0.0.0.0",
  heartbeat_timeout: 15,
});

const originalForm = ref<ConfigForm>({ ...form.value });
const saving = ref(false);
const saveMsg = ref("");
const saveMsgType = ref<"success" | "error">("success");

const computedStreamUrl = computed(() => {
  return `http://${form.value.pi_host}:${form.value.pi_stream_port}/video`;
});

const computedApiUrl = computed(() => {
  return `http://${form.value.pi_host}:${form.value.pi_api_port}/api/register`;
});

async function loadConfig() {
  try {
    const res = await getConfig();
    form.value = { ...form.value, ...res.data };
    originalForm.value = { ...form.value };
  } catch (err) {
    console.error("Load config failed:", err);
  }
}

async function saveConfig() {
  saving.value = true;
  saveMsg.value = "";
  try {
    await apiSaveConfig(form.value);
    originalForm.value = { ...form.value };
    saveMsg.value = "配置已保存！树莓派配置立即生效，服务器参数需重启后生效。";
    saveMsgType.value = "success";
    ElMessage.success("配置已保存");
  } catch (err: any) {
    const detail = err?.response?.data?.detail || err?.message || String(err);
    saveMsg.value = `保存失败：${detail}`;
    saveMsgType.value = "error";
    console.error("Save config failed:", err);
  } finally {
    saving.value = false;
  }
}

function resetForm() {
  form.value = { ...originalForm.value };
}

onMounted(loadConfig);
</script>

<style scoped>
.settings-page {
  max-width: 860px;
}

.page-header {
  margin-bottom: 24px;
}
.page-header h2 {
  font-size: 22px;
}
.page-desc {
  color: var(--text-secondary);
  font-size: 13px;
  margin-top: 4px;
}

.settings-cards {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 20px;
}

.setting-card {
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 24px;
}
.setting-card h3 {
  font-size: 16px;
  margin-bottom: 16px;
  color: var(--text-primary);
}
.card-icon {
  font-size: 28px;
  margin-bottom: 8px;
}

.setting-form {
  margin-top: 8px;
}

/* 暗色表单控件覆盖 */
.dark-input :deep(.el-input__wrapper),
.dark-number :deep(.el-input__wrapper) {
  background: rgba(124, 58, 237, 0.07) !important;
  border: 1px solid rgba(124, 58, 237, 0.2) !important;
  box-shadow: none !important;
  border-radius: 8px;
}
.dark-input :deep(.el-input__wrapper:hover),
.dark-number :deep(.el-input__wrapper:hover) {
  border-color: rgba(124, 58, 237, 0.4) !important;
}
.dark-input :deep(.el-input__wrapper.is-focus),
.dark-number :deep(.el-input__wrapper.is-focus) {
  border-color: var(--accent) !important;
  box-shadow: 0 0 0 2px rgba(124, 58, 237, 0.2) !important;
}
.dark-input :deep(.el-input__inner),
.dark-number :deep(.el-input__inner) {
  color: var(--text-primary);
}
.dark-number :deep(.el-input-number__decrease),
.dark-number :deep(.el-input-number__increase) {
  background: rgba(124, 58, 237, 0.1) !important;
  color: var(--accent) !important;
  border-color: rgba(124, 58, 237, 0.2) !important;
}

.form-hint {
  font-size: 11px;
  color: var(--text-secondary);
  margin-top: 2px;
  display: block;
}

.preview-bar {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px 20px;
  margin-top: 20px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.preview-item {
  display: flex;
  align-items: center;
  gap: 12px;
}
.preview-label {
  font-size: 13px;
  color: var(--text-secondary);
  white-space: nowrap;
  min-width: 80px;
}
.preview-url {
  font-family: 'Fira Code', 'Consolas', monospace;
  font-size: 13px;
  color: var(--accent);
  word-break: break-all;
}

.actions {
  margin-top: 24px;
  display: flex;
  gap: 12px;
}
.save-btn {
  background: linear-gradient(135deg, #7c3aed, #8b5cf6) !important;
  border: none !important;
  padding: 10px 32px !important;
  font-weight: 600;
}
.save-btn:hover {
  background: linear-gradient(135deg, #8b5cf6, #a78bfa) !important;
}
.reset-btn {
  background: transparent !important;
  border: 1px solid var(--border) !important;
  color: var(--text-secondary) !important;
}
.reset-btn:hover {
  border-color: var(--accent) !important;
  color: var(--text-primary) !important;
}

@media (max-width: 720px) {
  .settings-cards {
    grid-template-columns: 1fr;
  }
}
</style>
