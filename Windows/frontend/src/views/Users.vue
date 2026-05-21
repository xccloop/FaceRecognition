<template>
  <div class="users-page">
    <!-- 页头 -->
    <div class="page-header" v-motion-fade>
      <h2>人员管理</h2>
      <div class="header-actions">
        <el-input
          v-model="search"
          placeholder="搜索姓名..."
          prefix-icon="Search"
          class="search-input"
          clearable
          @input="onSearch"
        />
        <el-button
          type="primary"
          :icon="Upload"
          :loading="syncingAll"
          @click="syncAll"
          :disabled="unsyncedCount === 0"
        >
          同步全部到 Pi ({{ unsyncedCount }})
        </el-button>
      </div>
    </div>

    <!-- Pi 状态提示 -->
    <el-alert
      v-if="!piOnline"
      title="树莓派离线"
      type="warning"
      :closable="false"
      show-icon
      style="margin-bottom: 16px"
    >
      无法同步数据到树莓派，请检查 Pi 端网络连接。
    </el-alert>

    <!-- 卡片网格 -->
    <div class="user-grid" v-loading="loading">
      <transition-group name="card-list">
        <div
          v-for="user in users"
          :key="user.id"
          class="user-card card-hover"
          @click="viewUser(user)"
          v-motion-pop
        >
          <div class="card-photo">
            <el-image
              :src="user.photo_url || undefined"
              fit="cover"
              :preview-src-list="user.photo_url ? [user.photo_url] : []"
            >
              <template #error>
                <div class="photo-placeholder">
                  <el-icon :size="36"><UserFilled /></el-icon>
                </div>
              </template>
            </el-image>
            <!-- 同步状态角标 -->
            <div class="sync-badge" :class="{ synced: user.pi_synced }">
              <el-icon :size="12">
                <Check v-if="user.pi_synced" />
                <Clock v-else />
              </el-icon>
            </div>
          </div>
          <div class="card-info">
            <span class="card-name">{{ user.name }}</span>
            <span class="card-id">#{{ String(user.id).padStart(3, '0') }}</span>
            <span class="card-time">{{ formatTime(user.created_at) }}</span>
          </div>
          <el-button
            class="delete-btn"
            circle
            size="small"
            type="danger"
            @click.stop="confirmDelete(user)"
          >
            <el-icon><Delete /></el-icon>
          </el-button>
        </div>
      </transition-group>
    </div>

    <!-- 空状态 -->
    <el-empty
      v-if="!loading && !users.length"
      description="暂无注册人员"
    />

    <!-- 分页 -->
    <div class="pagination" v-if="total > pageSize">
      <el-pagination
        v-model:current-page="page"
        :page-size="pageSize"
        :total="total"
        background
        layout="prev, pager, next"
        @current-change="loadUsers"
      />
    </div>

    <!-- 详情 Drawer -->
    <el-drawer
      v-model="drawerVisible"
      :title="drawerUser?.name || '人员详情'"
      direction="rtl"
      size="420px"
    >
      <div class="user-detail" v-if="drawerUser">
        <el-image
          :src="drawerUser.photo_url || undefined"
          fit="contain"
          style="width: 100%; border-radius: 12px; margin-bottom: 20px"
        >
          <template #error>
            <div
              style="
                width: 100%;
                height: 240px;
                background: #1a1a2e;
                display: flex;
                align-items: center;
                justify-content: center;
                border-radius: 12px;
              "
            >
              <el-icon :size="64" color="#a0a0b0"><UserFilled /></el-icon>
            </div>
          </template>
        </el-image>

        <el-descriptions :column="1" border>
          <el-descriptions-item label="姓名">{{
            drawerUser.name
          }}</el-descriptions-item>
          <el-descriptions-item label="编号"
            >#{{ String(drawerUser.id).padStart(3, '0') }}</el-descriptions-item
          >
          <el-descriptions-item label="注册时间">{{
            formatTime(drawerUser.created_at)
          }}</el-descriptions-item>
          <el-descriptions-item label="已同步 Pi">
            <el-tag :type="drawerUser.pi_synced ? 'success' : 'info'" size="small">
              {{ drawerUser.pi_synced ? '已同步' : '未同步' }}
            </el-tag>
          </el-descriptions-item>
        </el-descriptions>

        <!-- 操作按钮组 -->
        <div class="drawer-actions">
          <el-button
            v-if="!drawerUser.pi_synced"
            type="primary"
            :loading="syncingId === drawerUser.id"
            style="flex: 1"
            @click="syncOne(drawerUser)"
          >
            同步到树莓派
          </el-button>
          <el-tag v-else type="success" style="flex: 1; text-align: center; padding: 8px 0">
            <el-icon style="margin-right: 4px"><Check /></el-icon>
            已同步到 Pi
          </el-tag>
          <el-button
            type="danger"
            style="flex: 1"
            @click="confirmDelete(drawerUser)"
          >
            删除此人
          </el-button>
        </div>
      </div>
    </el-drawer>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from "vue";
import { Delete, UserFilled, Upload, Check, Clock } from "@element-plus/icons-vue";
import { ElMessage, ElMessageBox } from "element-plus";
import { getUsers, deleteUser, syncUser, syncAllUsers, getPiStatus } from "../api";

interface UserItem {
  id: number;
  name: string;
  photo_url: string;
  created_at: string;
  pi_synced: boolean;
}

const users = ref<UserItem[]>([]);
const total = ref(0);
const page = ref(1);
const pageSize = ref(12);
const loading = ref(false);
const search = ref("");
const drawerVisible = ref(false);
const drawerUser = ref<UserItem | null>(null);
const syncingId = ref(0);
const syncingAll = ref(false);
const piOnline = ref(false);

const unsyncedCount = computed(() =>
  users.value.filter((u) => !u.pi_synced).length
);

let searchTimer: ReturnType<typeof setTimeout>;

async function loadUsers() {
  loading.value = true;
  try {
    const res = await getUsers(page.value, pageSize.value, search.value);
    users.value = res.data.items || [];
    total.value = res.data.total || 0;
  } catch (err) {
    console.error("Load users failed:", err);
    ElMessage.error("加载人员列表失败");
  } finally {
    loading.value = false;
  }
}

async function checkPiStatus() {
  try {
    const res = await getPiStatus();
    piOnline.value = res.data.pi_online === true;
  } catch {
    piOnline.value = false;
  }
}

async function syncOne(user: UserItem) {
  if (!piOnline.value) {
    ElMessage.warning("树莓派不在线，无法同步");
    return;
  }
  syncingId.value = user.id;
  try {
    await syncUser(user.id);
    ElMessage.success(`已将 ${user.name} 同步到树莓派`);
    await loadUsers();
  } catch (err: any) {
    ElMessage.error(err?.response?.data?.error || "同步失败");
  } finally {
    syncingId.value = 0;
  }
}

async function syncAll() {
  if (!piOnline.value) {
    ElMessage.warning("树莓派不在线，无法同步");
    return;
  }
  syncingAll.value = true;
  try {
    const res = await syncAllUsers();
    const data = res.data;
    ElMessage.success(data.message || "同步完成");
    await loadUsers();
  } catch (err: any) {
    ElMessage.error(err?.response?.data?.error || "同步失败");
  } finally {
    syncingAll.value = false;
  }
}

function onSearch() {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => {
    page.value = 1;
    loadUsers();
  }, 300);
}

function viewUser(user: UserItem) {
  drawerUser.value = user;
  drawerVisible.value = true;
}

async function confirmDelete(user: UserItem) {
  try {
    await ElMessageBox.confirm(
      `确认删除「${user.name}」？此操作不可撤销。`,
      "删除确认",
      {
        type: "warning",
        confirmButtonText: "删除",
        cancelButtonText: "取消",
      }
    );
    await deleteUser(user.id);
    ElMessage.success(`已删除 ${user.name}`);
    drawerVisible.value = false;
    await loadUsers();
  } catch {
    // 用户取消删除
  }
}

function formatTime(t: string): string {
  if (!t) return "";
  const diff = Date.now() - new Date(t).getTime();
  const mins = Math.floor(diff / 60000);
  if (mins < 1) return "刚刚";
  if (mins < 60) return `${mins}分钟前`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}小时前`;
  return `${Math.floor(hours / 24)}天前`;
}

onMounted(() => {
  loadUsers();
  checkPiStatus();
});
</script>

<style scoped>
.users-page {
  max-width: 1000px;
}

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 24px;
  flex-wrap: wrap;
  gap: 12px;
}
.page-header h2 {
  font-size: 22px;
}
.header-actions {
  display: flex;
  gap: 12px;
  align-items: center;
}
.search-input {
  width: 240px;
}
.search-input :deep(.el-input__wrapper) {
  background: rgba(124, 58, 237, 0.08) !important;
  border: 1px solid rgba(124, 58, 237, 0.25) !important;
  box-shadow: none !important;
  border-radius: 10px;
  transition: all 0.2s ease;
}
.search-input :deep(.el-input__wrapper:hover) {
  border-color: rgba(124, 58, 237, 0.45) !important;
  background: rgba(124, 58, 237, 0.12) !important;
}
.search-input :deep(.el-input__wrapper.is-focus) {
  border-color: var(--accent) !important;
  background: rgba(124, 58, 237, 0.14) !important;
  box-shadow: 0 0 0 3px rgba(124, 58, 237, 0.15) !important;
}
.search-input :deep(.el-input__inner) {
  color: var(--text-primary);
}
.search-input :deep(.el-input__inner::placeholder) {
  color: rgba(160, 160, 176, 0.5);
}
.search-input :deep(.el-icon) {
  color: rgba(124, 58, 237, 0.5);
}

.user-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 16px;
  min-height: 200px;
}

.user-card {
  background: var(--bg-card);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid var(--border);
  border-radius: 14px;
  overflow: hidden;
  position: relative;
  cursor: pointer;
}
.card-photo {
  width: 100%;
  height: 180px;
  background: #000;
  overflow: hidden;
  position: relative;
}
.card-photo img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  transition: transform 0.3s;
}
.user-card:hover .card-photo img {
  transform: scale(1.05);
}

/* 同步状态角标 */
.sync-badge {
  position: absolute;
  bottom: 8px;
  right: 8px;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: rgba(0, 0, 0, 0.6);
  display: flex;
  align-items: center;
  justify-content: center;
  color: #f0a020;
  transition: all 0.3s;
}
.sync-badge.synced {
  color: #67c23a;
}

.photo-placeholder {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #1a1a2e;
  color: #a0a0b0;
}

.card-info {
  padding: 14px;
}
.card-name {
  font-size: 15px;
  font-weight: 600;
  display: block;
  margin-bottom: 2px;
}
.card-id {
  font-size: 12px;
  color: var(--accent);
}
.card-time {
  font-size: 11px;
  color: var(--text-secondary);
  display: block;
  margin-top: 4px;
}

.delete-btn {
  position: absolute;
  top: 8px;
  right: 8px;
  opacity: 0;
  transition: opacity 0.2s;
}
.user-card:hover .delete-btn {
  opacity: 1;
}

.drawer-actions {
  display: flex;
  gap: 12px;
  margin-top: 20px;
}

.pagination {
  display: flex;
  justify-content: center;
  margin-top: 32px;
}

/* transition-group 动画 */
.card-list-enter-active {
  transition: all 0.4s ease;
}
.card-list-leave-active {
  transition: all 0.3s ease;
}
.card-list-enter-from {
  opacity: 0;
  transform: translateY(20px);
}
.card-list-leave-to {
  opacity: 0;
  transform: scale(0.9);
}
</style>
