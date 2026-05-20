import { createRouter, createWebHashHistory } from "vue-router";

const routes = [
  {
    path: "/",
    name: "Dashboard",
    component: () => import("../views/Dashboard.vue"),
  },
  {
    path: "/camera",
    name: "Camera",
    component: () => import("../views/Camera.vue"),
  },
  {
    path: "/users",
    name: "Users",
    component: () => import("../views/Users.vue"),
  },
];

const router = createRouter({
  history: createWebHashHistory(),
  routes,
});

export default router;
