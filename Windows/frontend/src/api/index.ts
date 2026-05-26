import axios from "axios";

const api = axios.create({
  baseURL: "/",
  timeout: 10000,
});

/** 获取仪表盘数据 */
export function getDashboard() {
  return api.get("/api/dashboard");
}

/** 获取人员列表 */
export function getUsers(page = 1, pageSize = 12, search = "") {
  return api.get("/api/users", { params: { page, page_size: pageSize, search } });
}

/** 删除人员（仅删数据库，保留照片） */
export function deleteUser(id: number) {
  return api.delete(`/api/users/${id}`);
}

/** 从树莓派删除特征（保留 Windows 数据） */
export function deleteUserFromPi(id: number) {
  return api.delete(`/api/users/${id}/pi`);
}

/** 获取摄像头状态 */
export function getCameraStatus() {
  return api.get("/api/camera/status");
}

/** 获取 Pi 连接状态 */
export function getPiStatus() {
  return api.get("/api/pi/status");
}

/** 注册人员（上传照片 + 姓名） */
export function registerUser(formData: FormData) {
  return api.post("/api/register", formData, {
    headers: { "Content-Type": "multipart/form-data" },
  });
}

/** 同步单个用户到树莓派 */
export function syncUser(userId: number) {
  return api.post(`/api/users/${userId}/sync`);
}

/** 批量同步所有未同步用户到树莓派 */
export function syncAllUsers() {
  return api.post("/api/users/sync-all");
}

/** 获取系统配置 */
export function getConfig() {
  return api.get("/api/config");
}

/** 保存系统配置 */
export function saveConfig(data: Record<string, unknown>) {
  return api.put("/api/config", data);
}
