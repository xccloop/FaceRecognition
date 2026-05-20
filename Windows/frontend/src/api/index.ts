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

/** 删除人员 */
export function deleteUser(id: number) {
  return api.delete(`/api/users/${id}`);
}

/** 获取摄像头状态 */
export function getCameraStatus() {
  return api.get("/api/camera/status");
}

/** 注册人员（上传照片 + 姓名） */
export function registerUser(formData: FormData) {
  return api.post("/api/register", formData, {
    headers: { "Content-Type": "multipart/form-data" },
  });
}
