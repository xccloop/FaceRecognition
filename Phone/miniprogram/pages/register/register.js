"use strict";

/**
 * register.js — 人脸采集注册页面逻辑 (v2: ES6 + 扫码 + 最近列表 + 连接测试)
 *
 * @summary 数据流：
 *   拍照/选图 → 输入姓名 → 上传到 Windows 后台 → 后台异步同步到 Pi
 *
 * @author 向治昌
 */

/* ================================================================
 * API 工具函数
 * ================================================================ */

/**
 * 规范化服务器 URL，仅保留协议+IP+端口
 * @param {string} input - 用户输入或扫码结果
 * @param {number} defaultPort - 当前默认端口
 * @returns {{ baseUrl: string, ip: string, port: number }|null}
 */
const normalizeUrl = (input, defaultPort) => {
  if (defaultPort === undefined) defaultPort = 8081;
  let s = (input || "").trim();
  if (!s) return null;

  // 去除协议前缀
  let protocol = "http";
  if (s.startsWith("http://")) { s = s.slice(7); }
  else if (s.startsWith("https://")) { protocol = "https"; s = s.slice(8); }

  // 去除路径和尾随斜杠
  const slashIdx = s.indexOf("/");
  if (slashIdx >= 0) s = s.slice(0, slashIdx);
  s = s.replace(/\/+$/, "");

  // 拆分 IP:端口
  let ip, port;
  if (s.includes(":")) {
    const parts = s.split(":");
    ip = parts[0];
    port = parseInt(parts[1], 10) || defaultPort;
  } else {
    ip = s;
    port = defaultPort;
  }

  if (!ip || port < 1 || port > 65535) return null;

  return { baseUrl: `${protocol}://${ip}:${port}`, ip, port };
};

/**
 * 从本地存储读取已保存的服务器配置
 * @returns {{ baseUrl: string, ip: string, port: number, timeout: number }|null}
 */
const getSavedConfig = () => {
  try {
    const config = wx.getStorageSync("server_config");
    if (config && config.baseUrl) return config;
  } catch (e) { /* ignore */ }
  return null;
};

/**
 * 获取服务器基地址
 * 优先级：本地存储 > app.globalData > 默认值
 * @returns {string}
 */
const getBaseUrl = () => {
  const saved = getSavedConfig();
  if (saved) return saved.baseUrl;
  try {
    const app = getApp();
    const config = app.globalData.serverConfig;
    if (config && config.baseUrl) return config.baseUrl;
  } catch (e) { /* ignore */ }
  return "http://192.168.137.1:8081";
};

/**
 * 获取请求超时（毫秒）
 * @returns {number}
 */
const getTimeout = () => {
  const saved = getSavedConfig();
  if (saved && saved.timeout) return saved.timeout;
  try {
    const app = getApp();
    const config = app.globalData.serverConfig;
    if (config && config.timeout) return config.timeout;
  } catch (e) { /* ignore */ }
  return 10000;
};

/** @returns {string} */
const registerUrl = () => getBaseUrl() + "/api/register";

/* ================================================================
 * 最近使用服务器列表
 * ================================================================ */

const RECENT_KEY = "recent_servers";
const MAX_RECENT = 3;

const getRecentServers = () => {
  try {
    const list = wx.getStorageSync(RECENT_KEY);
    return Array.isArray(list) ? list : [];
  } catch (e) { return []; }
};

const saveRecentServers = (list) => {
  wx.setStorageSync(RECENT_KEY, list.slice(0, MAX_RECENT));
};

/**
 * 将 baseUrl 添加到最近使用列表头部
 * @param {string} baseUrl
 */
const addToRecent = (baseUrl) => {
  const list = getRecentServers().filter(item => item.baseUrl !== baseUrl);
  list.unshift({ baseUrl, lastUsed: Date.now() });
  saveRecentServers(list);
};

/**
 * 提取服务器显示信息
 * @returns {{ full: string, display: string, configured: boolean }}
 */
const getServerDisplayInfo = () => {
  const url = getBaseUrl();
  const display = url.replace("http://", "").replace("https://", "");
  return { full: url, display, configured: !!getSavedConfig() };
};

/* ================================================================
 * Page 定义
 * ================================================================ */

Page({

  data: {
    /** @type {string} 临时照片路径 */
    photoPath: "",
    /** @type {string} 姓名 */
    name: "",
    /** @type {boolean} 姓名是否有效 */
    nameValid: false,
    /** @type {null|{success: boolean, message: string}} 提交结果 */
    result: null,
    /** @type {boolean} 是否正在提交 */
    submitting: false,

    /* ── 服务器设置 ── */
    showSettings: false,
    serverIp: "192.168.137.1",
    serverPort: "8081",
    testResult: null,
    testing: false,
    serverDisplay: "",
    serverConfigured: false,

    /* ── 最近使用列表 ── */
    recentServers: [],
  },

  /* ================================================================
   * 生命周期
   * ================================================================ */

  onLoad() {
    const info = getServerDisplayInfo();
    this.setData({
      serverDisplay: info.display,
      serverConfigured: info.configured,
      recentServers: getRecentServers(),
    });
  },

  onShow() {
    const info = getServerDisplayInfo();
    this.setData({
      serverDisplay: info.display,
      serverConfigured: info.configured,
      recentServers: getRecentServers(),
    });
  },

  /* ================================================================
   * 拍照 / 选图
   * ================================================================ */

  takePhoto() { this.selectMedia(["camera"]); },
  chooseFromAlbum() { this.selectMedia(["album"]); },

  choosePhoto() {
    wx.showActionSheet({
      itemList: ["拍照", "从相册选择"],
      success: (res) => {
        if (res.tapIndex === 0) this.takePhoto();
        else this.chooseFromAlbum();
      },
    });
  },

  /**
   * 先触发微信隐私弹窗，通过后再选图
   * @param {string[]} sourceType - ['camera'] | ['album']
   */
  selectMedia(sourceType) {
    wx.requirePrivacyAuthorize({
      success: () => { this._doChooseMedia(sourceType); },
      fail: () => {
        console.warn("[register] 用户未同意隐私协议");
        wx.showToast({ title: "请先同意隐私保护指引", icon: "none" });
      },
    });
  },

  /** @param {string[]} sourceType */
  _doChooseMedia(sourceType) {
    wx.chooseMedia({
      count: 1,
      mediaType: ["image"],
      sourceType,
      sizeType: ["compressed"],  // 微信单次压缩，不再二次压缩避免损识别率
      success: (res) => {
        const file = res.tempFiles[0];
        this.setData({ photoPath: file.tempFilePath, result: null });
        console.log(`[register] 已选照片: ${file.tempFilePath} 大小: ${file.size}`);
      },
      fail: (err) => {
        if (err.errMsg && err.errMsg.indexOf("cancel") === -1) {
          console.warn("[register] chooseMedia 失败:", err.errMsg);
          wx.showToast({ title: "获取图片失败", icon: "none" });
        }
      },
    });
  },

  /* ================================================================
   * 输入处理
   * ================================================================ */

  onNameInput(e) {
    const trimmed = e.detail.value.trim();
    this.setData({
      name: e.detail.value,
      nameValid: trimmed.length > 0,
    });
  },

  /* ================================================================
   * 表单提交
   * ================================================================ */

  submitRegister() {
    const photoPath = this.data.photoPath;
    const name = this.data.name;

    if (!photoPath) {
      wx.showToast({ title: "请先拍照或选择照片", icon: "none" });
      return;
    }

    const trimmedName = name.trim();
    if (!trimmedName) {
      wx.showToast({ title: "请输入姓名", icon: "none" });
      return;
    }

    this.setData({ submitting: true });
    wx.showLoading({ title: "上传中...", mask: true });

    wx.uploadFile({
      url: registerUrl(),
      filePath: photoPath,
      name: "photo",
      formData: { name: trimmedName },
      timeout: getTimeout(),
      success: (res) => {
        wx.hideLoading();
        this.setData({ submitting: false });

        if (res.statusCode !== 200) {
          this.setData({
            result: { success: false, message: `服务器错误 (HTTP ${res.statusCode})` },
          });
          return;
        }

        let data;
        try {
          data = JSON.parse(res.data);
        } catch (e) {
          console.error("[register] JSON 解析失败:", res.data);
          this.setData({ result: { success: false, message: "服务器响应格式错误" } });
          return;
        }

        if (data.success === true) {
          const syncHint = data.pi_sync_status === "pending"
            ? "\n(后台正在同步到门禁设备...)"
            : "";
          this.setData({
            result: { success: true, message: `注册成功：${trimmedName}${syncHint}` },
          });
          console.log("[register] 注册成功:", trimmedName, "sync:", data.pi_sync_status);
        } else {
          this.setData({
            result: { success: false, message: data.message || data.detail || "注册失败，请重试" },
          });
          console.warn("[register] 注册失败:", data.message);
        }
      },
      fail: (err) => {
        wx.hideLoading();
        this.setData({ submitting: false });
        console.error("[register] 上传失败:", err.errMsg);

        const errMsg = err.errMsg || "";
        let message = `网络错误，请检查 Windows 后台是否运行\n当前服务器：${getBaseUrl()}`;

        if (errMsg.indexOf("timeout") !== -1) {
          message = "上传超时，请检查网络连接";
        } else if (errMsg.indexOf("fail url not in domain list") !== -1) {
          message = "请求域名未配置，请在开发者工具中关闭域名校验";
        }

        this.setData({ result: { success: false, message } });
      },
    });
  },

  /* ================================================================
   * 重置
   * ================================================================ */

  reset() {
    this.setData({
      photoPath: "",
      name: "",
      nameValid: false,
      result: null,
      submitting: false,
    });
  },

  /* ================================================================
   * 服务器设置弹窗
   * ================================================================ */

  showSettingsDialog() {
    const saved = getSavedConfig();
    this.setData({
      showSettings: true,
      serverIp: (saved && saved.ip) || "192.168.137.1",
      serverPort: String((saved && saved.port) || 8081),
      testResult: null,
      testing: false,
      recentServers: getRecentServers(),
    });
  },

  hideSettingsDialog() {
    this.setData({ showSettings: false, testResult: null });
  },

  stopPropagation() { /* catchtap 阻止冒泡 */ },

  onIpInput(e) { this.setData({ serverIp: e.detail.value, testResult: null }); },
  onPortInput(e) { this.setData({ serverPort: e.detail.value, testResult: null }); },

  /**
   * 保存服务器配置 — 规范化 + 写入本地存储
   */
  saveServerConfig() {
    const normalized = normalizeUrl(
      `${this.data.serverIp}:${this.data.serverPort}`,
      parseInt(this.data.serverPort, 10) || 8081
    );
    if (!normalized) {
      wx.showToast({ title: "IP 或端口格式无效", icon: "none" });
      return;
    }

    const config = { baseUrl: normalized.baseUrl, ip: normalized.ip, port: normalized.port, timeout: 10000 };

    wx.setStorageSync("server_config", config);
    try {
      const app = getApp();
      if (app) app.globalData.serverConfig = config;
    } catch (e) { /* ignore */ }

    // 添加到最近使用列表
    addToRecent(config.baseUrl);

    this.setData({
      serverConfigured: true,
      serverDisplay: `${normalized.ip}:${normalized.port}`,
      showSettings: false,
      testResult: null,
    });

    wx.showToast({ title: "服务器地址已保存", icon: "success" });
  },

  /**
   * 测试连接 — HEAD /api/health
   */
  testConnection() {
    const normalized = normalizeUrl(
      `${this.data.serverIp}:${this.data.serverPort}`,
      parseInt(this.data.serverPort, 10) || 8081
    );
    if (!normalized) {
      wx.showToast({ title: "IP 或端口格式无效", icon: "none" });
      return;
    }

    this.setData({ testing: true, testResult: null });

    const testUrl = `${normalized.baseUrl}/api/health`;

    wx.request({
      url: testUrl,
      method: "HEAD",
      timeout: 5000,
      success: (res) => {
        this.setData({ testing: false });
        if (res.statusCode === 200) {
          this.setData({
            testResult: {
              success: true,
              message: `连接成功！服务器运行中 (端口 ${normalized.port})`,
            },
          });
        } else {
          this.setData({
            testResult: {
              success: false,
              message: `服务器返回非预期状态码 (HTTP ${res.statusCode})`,
            },
          });
        }
      },
      fail: (err) => {
        this.setData({ testing: false });
        let msg = `无法连接到 ${normalized.ip}:${normalized.port}`;
        if (err.errMsg && err.errMsg.indexOf("timeout") !== -1) {
          msg = "连接超时，请确认 IP 和端口正确，且 Windows 后台已启动";
        }
        this.setData({ testResult: { success: false, message: msg } });
      },
    });
  },

  /**
   * 扫码配置 — 解析 QR 码中的服务器地址
   */
  scanQRCode() {
    wx.scanCode({
      scanType: ["qrCode"],
      success: (res) => {
        const normalized = normalizeUrl(res.result);
        if (!normalized) {
          wx.showToast({ title: "二维码内容不是有效的服务器地址", icon: "none" });
          return;
        }
        this.setData({
          serverIp: normalized.ip,
          serverPort: String(normalized.port),
          testResult: null,
        });
        wx.showToast({ title: `已识别: ${normalized.ip}:${normalized.port}`, icon: "success" });
      },
      fail: (err) => {
        if (err.errMsg && err.errMsg.indexOf("cancel") === -1) {
          console.warn("[register] scanCode 失败:", err.errMsg);
        }
      },
    });
  },

  /**
   * 从最近列表选择一个服务器
   * @param {WechatMiniprogram.BaseEvent} e
   */
  selectRecentServer(e) {
    const idx = e.currentTarget.dataset.index;
    const list = getRecentServers();
    if (idx >= 0 && idx < list.length) {
      const normalized = normalizeUrl(list[idx].baseUrl);
      if (normalized) {
        this.setData({
          serverIp: normalized.ip,
          serverPort: String(normalized.port),
          testResult: null,
        });
      }
    }
  },

  /**
   * 删除最近列表中的一条记录
   * @param {WechatMiniprogram.BaseEvent} e
   */
  removeRecentServer(e) {
    const idx = e.currentTarget.dataset.index;
    const list = getRecentServers();
    if (idx >= 0 && idx < list.length) {
      list.splice(idx, 1);
      saveRecentServers(list);
      this.setData({ recentServers: list });
    }
  },

  /**
   * 清空全部最近记录（二次确认）
   */
  clearRecentServers() {
    wx.showModal({
      title: "确认清空",
      content: "将清除所有已保存的服务器地址，此操作不可恢复。",
      success: (res) => {
        if (res.confirm) {
          saveRecentServers([]);
          this.setData({ recentServers: [] });
          wx.showToast({ title: "已清空", icon: "success" });
        }
      },
    });
  },
});
