# Phone — 微信小程序（人脸采集入口）

## 技术栈

| 项目 | 说明 |
|------|------|
| 运行时 | 微信小程序原生框架 (基础库 ≥3.3.4) |
| 语言 | JavaScript ES6 |
| 页面 | 单页 (register) |
| 全局状态 | `App.globalData` + `wx.setStorageSync` |
| IDE | 微信开发者工具 |

## 功能

- 拍照或选图 → 输入姓名 → 上传到 Windows 后台注册
- 服务器地址配置（手动输入 / 扫码）
- 最近 3 条注册记录
- 连接测试

## 目录结构

```
Phone/
├── project.config.json          # 微信开发者工具项目配置
├── project.private.config.json  # 私有配置（不提交 Git）
└── miniprogram/
    ├── app.js                   # 入口，恢复服务器配置
    ├── app.json                 # 页面注册 + 窗口样式
    ├── app.wxss                 # 全局样式
    ├── pages/register/
    │   ├── register.js          # 核心逻辑（拍照/上传/扫码）
    │   ├── register.wxml        # 页面布局
    │   └── register.wxss        # 页面样式
    └── utils/
        └── api.js               # API 端点封装 + 服务器地址管理
```

## 开发

1. 微信开发者工具打开 `Phone/` 目录
2. 在设置页面输入 Windows 后台地址：`http://192.168.1.2:8081`
3. 确保手机和 Windows 在同一局域网
4. 编译 → 预览/真机调试

## 服务器地址

地址优先级：本地存储 (`wx.setStorageSync`) > 默认值 (`api.js`)

修改默认值：编辑 `miniprogram/utils/api.js` 中的 `getBaseUrl()`

## 隐私合规

- 照片仅通过内网 HTTP 传输，不经过外网
- `app.json` 开启 `__usePrivacyCheck__: true`
- 拍照前通过 `wx.requirePrivacyAuthorize()` 触发隐私授权弹窗
