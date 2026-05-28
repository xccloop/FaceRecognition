"use strict";

/**
 * api.js — Windows 后台 API 封装
 *
 * 职责：
 *   1. 统一管理 API 端点 URL
 *   2. 全局超时时间配置
 *   3. 未来可扩展添加请求/响应拦截
 *
 * ⚠️ 开发阶段：修改 BASE_URL 为你的 Windows 局域网 IP
 *    示例：http://192.168.1.100:8000
 *
 * @author 向治昌
 */

/**
 * 获取 App 实例（延迟调用，避免模块加载时 App 未初始化）
 * @returns {WechatMiniprogram.App.Instance}
 */
function _app() {
  return getApp();
}

/**
 * 获取服务器基地址
 *
 * 优先级：
 *   1. app.globalData.serverConfig.baseUrl（从本地存储恢复）
 *   2. 默认值
 *
 * @returns {string} 服务器基地址
 */
function getBaseUrl() {
  var config = _app().globalData.serverConfig;
  return (config && config.baseUrl) || "http://192.168.1.2:8081";
}

/**
 * 获取请求超时时间（毫秒）
 * @returns {number}
 */
function getTimeout() {
  var config = _app().globalData.serverConfig;
  return (config && config.timeout) || 10000;
}

module.exports = {

  /**
   * 人脸注册接口
   * POST /api/register
   * Body: multipart/form-data
   *   - photo: File
   *   - name: string
   * Response: { code: 0, name: "xxx" }
   *
   * @returns {string} 完整 URL
   */
  registerUrl() {
    return getBaseUrl() + "/api/register";
  },

  /**
   * 获取服务器配置
   * @returns {{ baseUrl: string, timeout: number }}
   */
  getConfig() {
    return {
      baseUrl: getBaseUrl(),
      timeout: getTimeout()
    };
  },

  /**
   * 更新服务器配置（保存到本地存储）
   * @param {string} baseUrl - 新的服务器地址
   * @param {number} [timeout] - 超时时间（ms）
   */
  setConfig(baseUrl, timeout) {
    const config = {
      baseUrl: baseUrl,
      timeout: timeout || getTimeout()
    };
    _app().globalData.serverConfig = config;
    wx.setStorageSync("server_config", config);
  },

  /** @returns {number} 超时时间（ms） */
  getTimeout: getTimeout,

  /** @returns {string} 服务器基地址 */
  getBaseUrl: getBaseUrl

};
