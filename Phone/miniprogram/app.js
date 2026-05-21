"use strict";

/**
 * 人脸采集小程序 — 应用入口
 *
 * 职责：
 *   1. 启动时从本地存储恢复配置（如上次使用的服务器 IP）
 *   2. 提供全局数据共享
 *
 * 隐私授权：通过 app.json 中 __usePrivacyCheck__: true 开启，
 * 具体授权在 register.js 中拍照前通过 wx.requirePrivacyAuthorize() 触发。
 *
 * @author 向治昌
 * @version 0.3.0
 */

App({
  /**
   * 小程序启动时触发
   */
  onLaunch: function () {
    // 从本地存储恢复服务器配置
    var cachedServer = wx.getStorageSync("server_config");
    this.globalData.serverConfig = cachedServer || {
      baseUrl: "http://192.168.1.5:8000",
      timeout: 10000
    };
  },

  /**
   * 全局数据（页面间共享）
   * @type {Object}
   */
  globalData: {
    /** @type {{ baseUrl: string, timeout: number }|null} 服务器配置 */
    serverConfig: null,
    /** @type {string|null} 当前管理员标识 */
    adminToken: null
  }
});
