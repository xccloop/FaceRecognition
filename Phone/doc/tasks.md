# 手机端（小程序）— 待完成事项

## 当前状态

尚未开始。需要一个简单的移动端应用，拍摄人脸照片并上传到 Windows 后台完成注册。

## 1. 技术选型

**推荐方案：微信小程序**

| 考量 | 选择 | 理由 |
|------|------|------|
| 平台 | 微信小程序 | 无需安装，扫码即用，iOS/Android 通用 |
| 框架 | 原生小程序框架 | 功能简单，无需引入额外框架 |
| UI | WeUI 或 Vant Weapp | 开箱即用的组件库 |
| 开发工具 | 微信开发者工具 | 官方 IDE，调试方便 |

**备选方案：uni-app** — 如果后续想扩展为独立 App 或 H5，可以用 uni-app 一套代码多端发布。

## 2. 页面结构

小程序只需 2 个页面：

### 页面 1：注册页 (`pages/register/register`)

```
┌──────────────────────┐
│    人脸信息注册        │
│                      │
│   ┌──────────────┐   │
│   │              │   │
│   │   摄像头预览   │   │
│   │   或照片预览   │   │
│   │              │   │
│   └──────────────┘   │
│                      │
│  [  拍照  ] [ 相册 ]  │
│                      │
│  姓名: [___________]  │
│  编号: [___________]  │  ← 可选，自动生成
│                      │
│  [     提交注册     ]  │
│                      │
│  提示: 请正对光线拍照   │
│  确保人脸清晰可见      │
└──────────────────────┘
```

### 页面 2：结果页 (`pages/result/result`)

```
┌──────────────────────┐
│    注册结果           │
│                      │
│        ✅ / ❌       │
│                      │
│   注册成功！          │
│   姓名: 张三          │
│   编号: 001           │
│                      │
│  [  继续注册  ]       │
└──────────────────────┘
```

## 3. 核心功能实现

### 3.1 拍照 / 选图

```javascript
// 拍照
takePhoto() {
    wx.chooseImage({
        count: 1,
        sizeType: ['compressed'],
        sourceType: ['camera'],
        success: (res) => {
            this.setData({ photoPath: res.tempFilePaths[0] });
        }
    });
},

// 从相册选择
chooseFromAlbum() {
    wx.chooseImage({
        count: 1,
        sizeType: ['compressed'],
        sourceType: ['album'],
        success: (res) => {
            this.setData({ photoPath: res.tempFilePaths[0] });
        }
    });
}
```

### 3.2 上传到 Windows 后台

```javascript
submitRegister() {
    const { photoPath, name } = this.data;
    if (!photoPath) { wx.showToast({ title: '请拍照', icon: 'none' }); return; }
    if (!name.trim()) { wx.showToast({ title: '请输入姓名', icon: 'none' }); return; }

    wx.showLoading({ title: '提交中...' });

    wx.uploadFile({
        url: 'http://你的Windows后台IP:8000/api/users/register',
        filePath: photoPath,
        name: 'photo',
        formData: {
            name: name.trim(),
            id: this.data.userId.trim() || ''  // 空则由后台自动生成
        },
        success: (res) => {
            wx.hideLoading();
            const data = JSON.parse(res.data);
            wx.navigateTo({
                url: `/pages/result/result?success=true&name=${data.name}&id=${data.id}`
            });
        },
        fail: (err) => {
            wx.hideLoading();
            wx.showToast({ title: '上传失败: ' + err.errMsg, icon: 'none' });
        }
    });
}
```

### 3.3 配置

```javascript
// config.js
const config = {
    // Windows 后台地址（开发时用局域网 IP，上线后替换为域名）
    apiBaseUrl: 'http://192.168.1.xxx:8000/api'
};
module.exports = config;
```

## 4. 项目结构

```
Phone/
├── miniprogram/
│   ├── app.js              # 小程序入口
│   ├── app.json            # 页面路由、窗口配置
│   ├── app.wxss            # 全局样式
│   ├── config.js           # API 地址配置
│   ├── pages/
│   │   ├── register/
│   │   │   ├── register.js
│   │   │   ├── register.wxml
│   │   │   └── register.wxss
│   │   └── result/
│   │       ├── result.js
│   │       ├── result.wxml
│   │       └── result.wxss
│   └── utils/
│       └── api.js          # 网络请求封装
├── project.config.json     # 小程序项目配置
└── doc/
    └── tasks.md
```

## 5. 注意事项

- **HTTPS 要求：** 微信小程序要求后台接口必须是 HTTPS。开发阶段可以在开发者工具中关闭域名校验（设置 → 不校验合法域名），上线后需要给 Windows 后台配置 HTTPS 证书。
- **图片压缩：** 上传前用 `sizeType: ['compressed']` 压缩照片，减少传输时间。
- **人脸质量提示：** 拍照时给出引导文字（正脸、光线充足、无遮挡），减少无效上传。
- **备案域名：** 正式上线需要已备案的域名指向 Windows 后台服务器。

## 6. 后续扩展

- **识别记录查看：** 增加一个页面，展示当前用户的识别日志（从 Windows 后台拉取）
- **通知推送：** 配合 STM32 端的动作（如开门），推送通知到手机
- **多人脸管理：** 一个账号可以管理多个注册人脸（如家庭成员）
