"use strict";

/**
 * register.js — 人脸采集注册页面逻辑
 *
 * 数据流：
 *   点击拍照 → wx.requirePrivacyAuthorize() 触发微信官方隐私弹窗
 *   用户同意 → wx.chooseMedia 获取照片
 *   输入姓名  → name 写入 data
 *   点击提交  → wx.uploadFile POST /api/register
 *   返回结果  → data.result 更新，渲染成功/失败 UI
 *
 * v0.3.0 新增：服务器设置弹窗（配置 Windows 后台 IP + 端口 + 连接测试）
 *
 * @author 向治昌
 */

/* ================================================================
 * 内联 API 工具函数（避免模块顶层 require() 在真机加载失败）
 * ================================================================ */

/**
 * 从本地存储读取已保存的服务器配置
 * @returns {{ baseUrl: string, ip: string, port: number, timeout: number }|null}
 */
function getSavedConfig() {
  try {
    var config = wx.getStorageSync("server_config");
    if (config && config.baseUrl) return config;
  } catch (e) {}
  return null;
}

/**
 * 获取服务器基地址
 * 优先级：本地存储 > app.globalData > 默认值
 * @returns {string}
 */
function getBaseUrl() {
  // 1. 优先从本地存储读取
  var saved = getSavedConfig();
  if (saved) return saved.baseUrl;

  // 2. 回退到全局数据
  try {
    var app = getApp();
    var config = app.globalData.serverConfig;
    if (config && config.baseUrl) return config.baseUrl;
  } catch (e) {}

  // 3. 最终回退
  return "http://192.168.1.5:8000";
}

/**
 * 获取请求超时时间（毫秒）
 * @returns {number}
 */
function getTimeout() {
  var saved = getSavedConfig();
  if (saved && saved.timeout) return saved.timeout;

  try {
    var app = getApp();
    var config = app.globalData.serverConfig;
    if (config && config.timeout) return config.timeout;
  } catch (e) {}

  return 10000;
}

/**
 * 人脸注册接口 URL
 * @returns {string}
 */
function registerUrl() {
  return getBaseUrl() + "/api/register";
}

/* ================================================================
 * 提取当前服务器地址信息（用于页面展示）
 * ================================================================ */

function getServerDisplayInfo() {
  var url = getBaseUrl();
  // 从 "http://192.168.1.5:8000" 提取显示文本
  var display = url.replace("http://", "").replace("https://", "");
  var hasConfig = !!getSavedConfig();
  return {
    full: url,
    display: display,
    configured: hasConfig
  };
}

/* ================================================================
 * Page 定义
 * ================================================================ */

Page({

  /**
   * 页面初始数据
   */
  data: {
    /** @type {string} 临时照片路径 */
    photoPath: "",
    /** @type {string} 姓名 */
    name: "",
    /** @type {boolean} 姓名是否有效（trim 后有内容） */
    nameValid: false,
    /** @type {null|{success: boolean, message: string}} 提交结果 */
    result: null,
    /** @type {boolean} 是否正在提交 */
    submitting: false,

    /* ── 服务器设置 ── */
    /** @type {boolean} 设置弹窗是否显示 */
    showSettings: false,
    /** @type {string} 设置弹窗中的 IP 输入 */
    serverIp: "192.168.1.5",
    /** @type {string} 设置弹窗中的端口输入 */
    serverPort: "8000",
    /** @type {null|{success: boolean, message: string}} 连接测试结果 */
    testResult: null,
    /** @type {boolean} 是否正在测试连接 */
    testing: false,
    /** @type {string} 页面顶部显示的服务器地址 */
    serverDisplay: "",
    /** @type {boolean} 是否已手动配置过服务器 */
    serverConfigured: false
  },

  /* ================================================================
   * 生命周期
   * ================================================================ */

  onLoad: function () {
    var info = getServerDisplayInfo();
    this.setData({
      serverDisplay: info.display,
      serverConfigured: info.configured
    });
  },

  onShow: function () {
    // 每次页面显示时刷新服务器地址（用户可能从设置改了配置）
    var info = getServerDisplayInfo();
    this.setData({
      serverDisplay: info.display,
      serverConfigured: info.configured
    });
  },

  /* ================================================================
   * 拍照 / 选图
   * ================================================================ */

  /**
   * 从摄像头拍照
   */
  takePhoto: function () {
    this.selectMedia(["camera"]);
  },

  /**
   * 从相册选图
   */
  chooseFromAlbum: function () {
    this.selectMedia(["album"]);
  },

  /**
   * 点击照片区域 — 弹出选择面板
   */
  choosePhoto: function () {
    var that = this;
    wx.showActionSheet({
      itemList: ["拍照", "从相册选择"],
      success: function (res) {
        if (res.tapIndex === 0) {
          that.takePhoto();
        } else {
          that.chooseFromAlbum();
        }
      }
    });
  },

  /**
   * 入口：先触发微信官方隐私弹窗，通过后再选图
   *
   * wx.requirePrivacyAuthorize() 是微信官方 API，会自动：
   *   1. 检查用户是否已同意隐私协议
   *   2. 若未同意，弹出微信原生隐私保护指引弹窗
   *   3. 弹窗中自动声明 chooseMedia 等权限范围
   *   4. 用户同意后 resolve，不同意则 reject
   *
   * @param {string[]} sourceType - ['camera'] | ['album']
   */
  selectMedia: function (sourceType) {
    var that = this;

    wx.requirePrivacyAuthorize({
      success: function () {
        // 用户已同意隐私协议，执行选图
        that._doChooseMedia(sourceType);
      },
      fail: function () {
        console.warn("[register] 用户未同意隐私协议");
        wx.showToast({ title: "请先同意隐私保护指引", icon: "none" });
      }
    });
  },

  /**
   * 实际调用 wx.chooseMedia（隐私授权通过后执行）
   * @param {string[]} sourceType
   * @private
   */
  _doChooseMedia: function (sourceType) {
    var that = this;
    wx.chooseMedia({
      count: 1,
      mediaType: ["image"],
      sourceType: sourceType,
      sizeType: ["compressed"],
      success: function (res) {
        var file = res.tempFiles[0];
        that.setData({
          photoPath: file.tempFilePath,
          result: null   // 清除上次结果
        });
        console.log("[register] 已选照片:", file.tempFilePath, "大小:", file.size);
      },
      fail: function (err) {
        // 用户取消操作不提示
        if (err.errMsg && err.errMsg.indexOf("cancel") === -1) {
          console.warn("[register] chooseMedia 失败:", err.errMsg);
          wx.showToast({ title: "获取图片失败", icon: "none" });
        }
      }
    });
  },

  /* ================================================================
   * 输入处理
   * ================================================================ */

  /**
   * 姓名输入事件
   * @param {WechatMiniprogram.Input} e
   */
  onNameInput: function (e) {
    var trimmed = e.detail.value.trim();
    this.setData({
      name: e.detail.value,
      nameValid: trimmed.length > 0
    });
  },

  /* ================================================================
   * 表单提交
   * ================================================================ */

  /**
   * 提交注册 — 表单验证 + 文件上传
   */
  submitRegister: function () {
    var that = this;
    var photoPath = this.data.photoPath;
    var name = this.data.name;

    // 表单验证
    if (!photoPath) {
      wx.showToast({ title: "请先拍照或选择照片", icon: "none" });
      return;
    }

    var trimmedName = name.trim();
    if (!trimmedName) {
      wx.showToast({ title: "请输入姓名", icon: "none" });
      return;
    }

    if (trimmedName.length < 1) {
      wx.showToast({ title: "姓名不能为空", icon: "none" });
      return;
    }

    // 进入提交状态
    this.setData({ submitting: true });
    wx.showLoading({ title: "上传中...", mask: true });

    // 上传到 Windows 后台
    wx.uploadFile({
      url: registerUrl(),
      filePath: photoPath,
      name: "photo",
      formData: { name: trimmedName },
      timeout: getTimeout(),
      success: function (res) {
        wx.hideLoading();
        that.setData({ submitting: false });

        // 检查 HTTP 状态码
        if (res.statusCode !== 200) {
          that.setData({
            result: {
              success: false,
              message: "服务器错误 (HTTP " + res.statusCode + ")"
            }
          });
          return;
        }

        // 解析响应
        var data;
        try {
          data = JSON.parse(res.data);
        } catch (e) {
          console.error("[register] JSON 解析失败:", res.data);
          that.setData({
            result: { success: false, message: "服务器响应格式错误" }
          });
          return;
        }

        // 判断业务状态码
        if (data.success === true) {
          that.setData({
            result: {
              success: true,
              message: "注册成功：" + trimmedName
            }
          });
          console.log("[register] 注册成功:", trimmedName);
        } else {
          that.setData({
            result: {
              success: false,
              message: data.message || data.detail || "注册失败，请重试"
            }
          });
          console.warn("[register] 注册失败:", data.message);
        }
      },
      fail: function (err) {
        wx.hideLoading();
        that.setData({ submitting: false });

        console.error("[register] 上传失败:", err.errMsg);

        var errMsg = err.errMsg || "";
        var message = "网络错误，请检查 Windows 后台是否运行\n当前服务器：" + getBaseUrl();

        if (errMsg.indexOf("timeout") !== -1) {
          message = "上传超时，请检查网络连接";
        } else if (errMsg.indexOf("fail url not in domain list") !== -1) {
          message = "请求域名未配置，请在开发者工具中关闭域名校验";
        }

        that.setData({
          result: { success: false, message: message }
        });
      }
    });
  },

  /* ================================================================
   * 重置 — 继续注册下一个人
   * ================================================================ */

  /**
   * 清空表单，准备下一次注册
   */
  reset: function () {
    this.setData({
      photoPath: "",
      name: "",
      nameValid: false,
      result: null,
      submitting: false
    });
  },

  /* ================================================================
   * 服务器设置弹窗（v0.3.0 新增）
   * ================================================================ */

  /**
   * 打开设置弹窗 — 加载当前配置
   */
  showSettingsDialog: function () {
    var saved = getSavedConfig();
    this.setData({
      showSettings: true,
      serverIp: (saved && saved.ip) || "192.168.1.5",
      serverPort: String((saved && saved.port) || 8000),
      testResult: null,
      testing: false
    });
  },

  /**
   * 关闭设置弹窗
   */
  hideSettingsDialog: function () {
    this.setData({ showSettings: false, testResult: null });
  },

  /**
   * 阻止弹窗内容点击冒泡到遮罩层
   */
  stopPropagation: function () {
    // 空函数，仅用于 catchtap 阻止冒泡
  },

  /**
   * IP 输入事件
   * @param {WechatMiniprogram.Input} e
   */
  onIpInput: function (e) {
    this.setData({ serverIp: e.detail.value, testResult: null });
  },

  /**
   * 端口输入事件
   * @param {WechatMiniprogram.Input} e
   */
  onPortInput: function (e) {
    this.setData({ serverPort: e.detail.value, testResult: null });
  },

  /**
   * 保存服务器配置 — 写入本地存储 + 更新全局数据
   */
  saveServerConfig: function () {
    var ip = this.data.serverIp.trim();
    var portNum = parseInt(this.data.serverPort, 10);

    // 验证 IP
    if (!ip) {
      wx.showToast({ title: "请输入服务器 IP 地址", icon: "none" });
      return;
    }
    // 验证端口
    if (!portNum || portNum < 1 || portNum > 65535) {
      wx.showToast({ title: "端口范围 1 - 65535", icon: "none" });
      return;
    }

    var baseUrl = "http://" + ip + ":" + portNum;
    var config = {
      baseUrl: baseUrl,
      ip: ip,
      port: portNum,
      timeout: 10000
    };

    // 保存到本地存储
    wx.setStorageSync("server_config", config);

    // 同步更新全局数据
    try {
      var app = getApp();
      if (app) {
        app.globalData.serverConfig = config;
      }
    } catch (e) {}

    // 更新页面显示
    this.setData({
      serverConfigured: true,
      serverDisplay: ip + ":" + portNum,
      showSettings: false,
      testResult: null
    });

    wx.showToast({ title: "服务器地址已保存", icon: "success" });
  },

  /**
   * 测试服务器连接 — GET /api/config
   */
  testConnection: function () {
    var that = this;
    var ip = this.data.serverIp.trim();
    var portNum = parseInt(this.data.serverPort, 10);

    // 验证输入
    if (!ip) {
      wx.showToast({ title: "请输入服务器 IP 地址", icon: "none" });
      return;
    }
    if (!portNum || portNum < 1 || portNum > 65535) {
      wx.showToast({ title: "端口范围 1 - 65535", icon: "none" });
      return;
    }

    that.setData({ testing: true, testResult: null });

    var testUrl = "http://" + ip + ":" + portNum + "/api/config";

    wx.request({
      url: testUrl,
      method: "GET",
      timeout: 5000,
      success: function (res) {
        that.setData({ testing: false });
        if (res.statusCode === 200) {
          that.setData({
            testResult: {
              success: true,
              message: "连接成功！服务器运行中 (端口 " + portNum + ")"
            }
          });
        } else {
          that.setData({
            testResult: {
              success: false,
              message: "服务器返回非预期状态码 (HTTP " + res.statusCode + ")"
            }
          });
        }
      },
      fail: function (err) {
        that.setData({ testing: false });
        var msg = "无法连接到 " + ip + ":" + portNum;
        if (err.errMsg && err.errMsg.indexOf("timeout") !== -1) {
          msg = "连接超时，请确认 IP 和端口正确，且 Windows 后台已启动";
        }
        that.setData({
          testResult: { success: false, message: msg }
        });
      }
    });
  }

});
