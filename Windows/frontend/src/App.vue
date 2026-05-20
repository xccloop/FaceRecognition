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
import { ref } from "vue";
import AppSidebar from "./components/AppSidebar.vue";

const collapsed = ref(false);
</script>

<style scoped>
.app-layout {
  display: flex;
  height: 100vh;
}
.main-area {
  flex: 1;
  overflow-y: auto;
  padding: 32px;
  margin-left: var(--sidebar-width);
  transition: margin-left 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
.main-area.expanded {
  margin-left: var(--sidebar-collapsed);
}
</style>
