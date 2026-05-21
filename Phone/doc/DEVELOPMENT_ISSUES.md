# Phone 端 — 开发问题与解决方案

> 记录微信小程序开发过程中遇到的典型问题、根因分析和解决方案。
> 持续更新，方便后续开发者和维护者快速定位。

---

## 目录

1. [环境搭建类](#一环境搭建类)
2. [网络请求类](#二网络请求类)
3. [文件上传类](#三文件上传类)
4. [UI 与体验类](#四ui-与体验类)
5. [调试与发布类](#五调试与发布类)

---

## 一、环境搭建类

### 1.1 微信开发者工具无法识别项目

**现象**：拖入项目文件夹后，开发者工具报"未找到 app.json"。

**根因**：
- `project.config.json` 中 `miniprogramRoot` 路径配置错误
- 项目文件不在声明的根目录下

**解决方案**：
```json
// project.config.json 中确保：
{
  "miniprogramRoot": "miniprogram/",
  "appid": "你的AppID"
}
```
确认 `miniprogram/app.json` 文件存在且格式正确。

**预防措施**：
- 建项目时严格按照微信官方目录结构
- `miniprogramRoot` 指向的目录必须包含 `app.json`

---

### 1.2 AppID 未填写导致真机预览失败

**现象**：模拟器可运行，但点击"预览"扫码后在手机端白屏或提示"未注册"。

**根因**：`project.config.json` 中 `appid` 为空字符串或使用了测试号。

**解决方案**：
1. 前往 [mp.weixin.qq.com](https://mp.weixin.qq.com) 注册小程序（个人即可）
2. 在"开发 → 开发管理 → 开发设置"中复制 AppID
3. 填入 `project.config.json` 的 `appid` 字段

**预防措施**：
- 使用测试号仅适合入门 demo，正式项目必须使用真实 AppID
- 将 `project.config.json` 中的 `appid` 置空提交（避免泄露），本地开发时填入

---

### 1.3 图片资源不显示

**现象**：页面上 `<image src="/assets/icons/camera.svg">` 显示空白。

**根因**：
- 小程序不支持直接引用 SVG 作为 `<image>` 的 src
- 文件路径大小写不匹配（开发工具 Windows 不区分大小写，真机区分）

**解决方案**：
- **方案 A**：将 SVG 转换为 base64 内联到 `<image>` 的 src
- **方案 B**：使用 PNG/JPG 替代 SVG
- **方案 C**：使用 iconfont 或微信自带图标
- **推荐**：使用文本 emoji 替代简单图标 (📷 ✅ ❌)

```xml
<!-- 不推荐 -->
<image src="/assets/icons/camera.svg" />

<!-- 推荐 -->
<text class="icon-text">📷</text>
```

**预防措施**：
- 小程序中简单图标优先用 emoji 或 unicode 字符
- 复杂图标用 iconfont 或 base64 data URI

---

## 二、网络请求类

### 2.1 "不在以下 request 合法域名列表中"

**现象**：wx.uploadFile 请求报错 `errMsg: "uploadFile:fail url not in domain list"`。

**根因**：小程序要求所有网络请求的域名必须在小程序管理后台配置为合法域名，且必须是 HTTPS。开发阶段的局域网 HTTP 地址自然不在白名单中。

**解决方案（开发期）**：
```
微信开发者工具 → 右上角"详情" → "本地设置"
  ✅ 勾选"不校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书"
```

**解决方案（上线期）**：
1. 为 Windows 后台配置 HTTPS 证书（可用 Let's Encrypt / acme.sh）
2. Nginx 反向代理，将 HTTPS 请求转发到 FastAPI
3. 在小程序管理后台 → 开发 → 开发管理 → 服务器域名 → 添加 request 和 uploadFile 合法域名

**预防措施**：
- 开发阶段始终开启"不校验域名"开关
- README 中明确标注此步骤

---

### 2.2 局域网 IP 无法访问

**现象**：手机扫码预览时上传失败，提示"网络错误"。

**根因**：
| 场景 | 原因 |
|------|------|
| 手机和 PC 不在同一 WiFi | 不同子网，IP 不可达 |
| Windows 防火墙拦截 | 入站规则未开放 8000 端口 |
| AP 隔离（企业/商场 WiFi） | 路由器禁止客户端间通信 |
| 使用了 localhost/127.0.0.1 | 回环地址只能本机访问 |

**解决方案**：
```bash
# 1. 确认 Windows 后台监听所有网卡（0.0.0.0，非 127.0.0.1）
uvicorn main:app --host 0.0.0.0 --port 8000

# 2. 查看 Windows 局域网 IP
ipconfig | findstr "IPv4"

# 3. 手机浏览器访问 http://<IP>:8000/docs 验证连通性

# 4. Windows 防火墙放行端口（管理员 PowerShell）：
New-NetFirewallRule -DisplayName "FaceRecognition API" `
  -Direction Inbound -LocalPort 8000 -Protocol TCP -Action Allow
```

**预防措施**：
- FastAPI 启动脚本始终用 `--host 0.0.0.0`
- 文档中详细说明防火墙配置步骤

---

### 2.3 请求超时

**现象**：上传大照片时，30 秒后报 `"uploadFile:fail timeout"`。

**根因**：
- 小程序默认超时时间短（约 30s）
- 照片未压缩，文件过大导致传输慢
- Windows 后台处理慢（如磁盘 IO 阻塞）

**解决方案**：

```javascript
// 拍照时启用压缩
wx.chooseMedia({
  sizeType: ["compressed"],  // ✅ 使用压缩
  // sizeType: ["original"],  // ❌ 避免原图
});

// 上传时显式设置超时
wx.uploadFile({
  url: api.registerUrl(),
  filePath: photoPath,
  name: "photo",
  formData: { name: "张三" },
  timeout: 15000  // ✅ 15 秒超时
});
```

**预防措施**：
- 图片始终使用压缩模式（compressed），微信压缩算法效果良好
- 上传超时设置为 10-15 秒，覆盖 95% 的正常情况
- 超过 5MB 的照片在客户端二次压缩后上传

---

## 三、文件上传类

### 3.1 wx.uploadFile 与 wx.request 的选择

**问题**：用 wx.uploadFile 还是 wx.request + Base64？

**分析**：

| 维度 | wx.uploadFile | wx.request + Base64 |
|------|---------------|---------------------|
| 实现复杂度 | 简单（原生表单） | 需要客户端 Base64 编码 + 服务端解码 |
| 传输体积 | 原始大小 | +33%（Base64 膨胀） |
| 内存占用 | 流式读取，低 | 需要加载完整 base64 字符串到内存 |
| 大文件支持 | 原生支持 | 受 wx.request 的 data 大小限制 |
| 服务端兼容 | FastAPI UploadFile 原生支持 | 需手动处理 JSON → bytes |

**结论**：**始终使用 wx.uploadFile**，除非需要自定义请求头或特殊字段格式。

---

### 3.2 上传后服务端收不到文件

**现象**：`wx.uploadFile` 返回 200，但 Windows 后台 `UploadFile` 为空。

**根因**：
- `name` 参数与服务端期望的字段名不匹配
- 服务端路由使用了错误的参数类型（如 `Form` 而非 `File`）

```python
# ❌ 错误：参数类型是 Form 而非 UploadFile
@app.post("/api/register")
async def register(photo: str = Form(...), name: str = Form(...)):
    ...

# ✅ 正确：文件类型使用 UploadFile
@app.post("/api/register")
async def register(photo: UploadFile = File(...), name: str = Form(...)):
    ...
```

**排查方法**：
```javascript
// 在 submitRegister() 中添加调试日志
console.log("[register] 上传参数:", {
  url: api.registerUrl(),
  filePath: this.data.photoPath,
  name: "photo",
  formData: { name: trimmedName }
});
```

**预防措施**：
- 前后端约定好字段名并写入接口文档
- 服务端对空文件/缺失字段返回明确错误信息

---

### 3.3 文件路径临时性

**现象**：在 Page 的 `onLoad` 中保存 `photoPath`，下次进入页面时图片失效。

**根因**：`wx.chooseMedia` 返回的 `tempFilePath` 是临时路径，小程序随时可能清理。

**解决方案**：
- 拍照后应立即上传，不要在本地保存 `tempFilePath` 到 `storage`
- 如需本地缓存，使用 `wx.saveFile` 获取持久化路径

```javascript
// ✅ 拍照后立即上传
takePhoto() {
  wx.chooseMedia({
    success: (res) => {
      // 注意：此时 tempFilePath 有效，但不要跨页面传递
      this.setData({ photoPath: res.tempFiles[0].tempFilePath });
    }
  });
}
```

---

## 四、UI 与体验类

### 4.1 disabled 属性不联动

**现象**：选择了照片并输入姓名后，提交按钮仍为灰色 disabled 状态。

**根因**：
- WXML 中 `disabled` 表达式未响应数据变化
- `setData` 时使用了错误的数据路径

```xml
<!-- ✅ 正确：disabled 绑定表达式 -->
<button disabled="{{!photoPath || !name.trim()}}">提交</button>

<!-- ❌ 错误：字符串不会更新 -->
<button disabled="true">提交</button>
```

**调试方法**：
```javascript
// 在 onNameInput 中添加日志
onNameInput(e) {
  this.setData({ name: e.detail.value });
  console.log("当前状态:", {
    photoPath: this.data.photoPath,
    name: this.data.name,
    canSubmit: !!(this.data.photoPath && this.data.name.trim())
  });
}
```

**预防措施**：
- `disabled` 使用数据绑定表达式而非写死布尔值
- 开发时在页面底部临时显示 `{{photoPath}}` `{{name}}` 辅助调试

---

### 4.2 成功/失败提示样式不显示

**现象**：上传返回后，`wx:if="{{result}}"` 区域不渲染。

**根因**：
- `result` 初始值设为 `null` 后，成功时 `setData({ result: {...} })` 应触发渲染
- 可能是 `setData` 中的对象键名拼写错误
- 或者是 CSS `display: none` 覆盖了 `wx:if`

**解决方案**：
```javascript
// 确保 setData 结构正确
this.setData({
  result: {
    success: true,
    message: `注册成功：${name}`
  }
});

// 检查 WXML 中的条件渲染
<view wx:if="{{result}}" class="result-banner {{result.success ? 'result-success' : 'result-error'}}">
```

**预防措施**：
- 使用微信开发者工具的 AppData 面板实时查看 data 变化
- 写模板前先用简单变量 (`{{result}}`) 测试数据绑定是否生效

---

### 4.3 图片预览区域比例失调

**现象**：横拍照片在预览区中被拉伸或留有大量空白。

**根因**：`<image>` 的 `mode` 属性设置不当。

| mode | 行为 |
|------|------|
| `aspectFit` | 保持比例，完整显示（可能留白） ✅ 推荐 |
| `aspectFill` | 保持比例，填满容器（可能裁切） |
| `scaleToFill` | 拉伸填满，不保持比例 ❌ |
| `widthFix` | 宽度不变，高度自动 |

```xml
<!-- 推荐：保持比例，完整显示 -->
<image src="{{photoPath}}" mode="aspectFit" class="photo-image" />
```

```css
/* 预览区使用固定高度 + 黑色背景 */
.photo-area {
  height: 500rpx;
  background: #000;  /* 照片间隙显示黑色 */
}
```

**预防措施**：
- 照片预览始终使用 `mode="aspectFit"`
- 预览区设置固定宽高比 + 深色背景

---

## 五、调试与发布类

### 5.1 开发者工具与真机行为不一致

**现象**：模拟器上一切正常，真机扫码预览时出现异常。

**常见差异**：

| 场景 | 模拟器 | 真机 | 原因 |
|------|--------|------|------|
| HTTP 请求 | 可访问 localhost | 只能访问局域网 IP | localhost 指向手机自身 |
| 文件路径 | 不区分大小写 | 区分大小写 | iOS 文件系统大小写敏感 |
| CSS 渲染 | 使用 Chrome 内核 | 使用微信 X5/WebKit | 浏览器引擎差异 |
| 摄像头权限 | 无需授权 | 需要用户授权 | 系统权限模型 |

**解决方案**：
- 开发阶段定时用真机预览测试
- 所有文件路径使用一致的大小写规范（推荐全小写 + 连字符）
- 使用 `wx.getSystemInfo()` 检测平台做兼容

---

### 5.2 console.log 不显示（Release 模式）

**现象**：真机调试时 `console.log` 无输出。

**根因**：小程序在非调试模式下会屏蔽 `console.log`（性能优化）。

**解决方案**：
```javascript
// 封装一个全局 logger
const logger = {
  log(...args) {
    if (typeof __wxConfig !== "undefined" && __wxConfig.debug) {
      console.log("[Phone]", ...args);
    }
  },
  error(...args) {
    console.error("[Phone]", ...args);  // error 始终输出
  }
};
```

**预防措施**：
- 关键流程用 `console.error` 或 `console.warn`（不会被屏蔽）
- 复杂联调问题用真机调试模式的 vConsole

---

### 5.3 体验版审核被拒

**常见被拒原因及应对**：

| 拒绝原因 | 解决方法 |
|----------|----------|
| 功能不完整（只有 UI 没有后端） | 提交审核前确保服务器在线且功能可用 |
| 未配置合法域名 | 小程序后台添加已备案的 HTTPS 域名 |
| 类目不匹配 | 选择合适的服务类目（工具 → 信息查询 等） |
| 收集用户信息无隐私协议 | 在设置页添加隐私政策链接 |
| 测试账号未提供 | 审核备注中提供测试账号和密码 |

---

## 六、真机兼容性类

### 6.1 真机只显示标题，主体内容空白

**现象**：模拟器预览正常，真机扫码后只显示导航栏标题"人脸采集"，页面主体完全空白。

**根因**（多个叠加）：
1. **CSS 混入 JS 代码** — `register.wxss` 第一行误写入了 `"use strict";`，导致 CSS 解析失败，所有样式失效
2. **顶层 getApp() 调用** — `utils/api.js` 模块顶层执行 `const app = getApp()`，在小程序模块加载阶段 App 尚未初始化，返回 undefined，后续 `app.globalData` 访问直接抛异常，页面 JS 执行中断
3. **require 链加载失败** — 真机远程编译环境下，`require("../../utils/api")` 可能因模块加载时序或转译问题返回空对象，调用 undefined 函数导致崩溃
4. **WXML 结构损坏** — sed 编辑工具误操作删除了 WXML 头部注释及后续行，导致根组件无法创建，报错 `rootCompPath=pages/register/register not found`

**解决方案**：
1. 从 wxss 中移除所有非 CSS 内容（`"use strict"` 等 JS 指令）
2. 将 `getApp()` 调用延迟到函数体内部：`function _app() { return getApp(); }`，使用时调用 `_app().globalData`
3. 将 API 函数（uploadFace, getBaseUrl 等）内联到 register.js，消除 `require` 依赖
4. 重写完整的 WXML 文件，避免使用文本编辑工具的批量替换

**预防**：
- 不要在 .wxss 文件中写入任何 JavaScript 代码
- 模块顶层避免调用 `getApp()`，始终在函数体内延迟获取
- 优先将工具函数内联到页面文件中，减少模块依赖
- 对关键文件做修改后，立即检查文件完整性（行数、关键结构）


### 6.2 ES6 语法真机不兼容

**现象**：模拟器正常，真机白屏或功能异常。

**根因**：微信小程序真机远程编译对 ES6 语法的转译不完整，以下写法可能失败：
- 箭头函数 `() => {}`
- 模板字符串 `` `url/${id}` ``
- `const` / `let` 声明
- `Array.prototype.includes()`
- 对象方法简写 `onLaunch() {}`

**解决方案**：全部转为 ES5 兼容写法：
```javascript
// 箭头函数 → function
[1,2,3].map(function(x) { return x * 2; });

// 模板字符串 → 字符串拼接
var url = baseUrl + "/api/register?id=" + id;

// const/let → var
var that = this;

// .includes() → .indexOf()
if (res.tempFiles[0].tempFilePath.indexOf("tmp") !== -1) { ... }

// 对象方法简写 → function 属性
onLaunch: function() { ... }
```

**预防**：项目全部使用 ES5 语法编写，在 `project.config.json` 中设置 `"es6": false`（关闭 ES6→ES5 转译以使用原始 ES5 代码，避免双重转译引入 bug）。


### 6.3 远程编译配置陷阱

**现象**：真机预览时编译失败或出现诡异错误。

**根因**：
- `"lazyCodeLoading": "requiredComponents"` — 远程编译环境下可能无法正确解析懒加载组件
- `"minified": true` — 代码压缩可能引入运行时 bug
- `"es6": true` 配合 ES6 代码 — 转译不完整

**解决方案**：
```json
// project.config.json
{
  "setting": {
    "es6": false,
    "minified": false,
    "minifyWXSS": false,
    "minifyWXML": false
  }
}

// app.json 中移除
// "lazyCodeLoading": "requiredComponents"
```

### 6.4 微信隐私保护指引弹窗

**现象**：控制台报 `private_getBackgroundFetchData:fail invalid request data`，提示"小程序还没有实现了用户隐私保护指引的逻辑"。真机上调用摄像头/相册等隐私 API 前被微信拦截。

**根因**：微信 2023年9月15日起强制要求：所有涉及用户隐私的 API（`wx.chooseMedia`、`wx.getLocation` 等）调用前，必须先让用户在隐私弹窗中点击"同意"。未实现则隐私 API 调用失败。

**解决方案**：

1. `app.json` 中启用隐私检查：
```json
{
  "__usePrivacyCheck__": true
}
```

2. `app.js` 中注册 `wx.onNeedPrivacyAuthorization` 监听器：
```javascript
wx.onNeedPrivacyAuthorization(function (resolve) {
  wx.showModal({
    title: "隐私保护指引",
    content: "隐私协议文本...",
    confirmText: "同意",
    cancelText: "不同意",
    success: function (res) {
      if (res.confirm) {
        resolve({ buttonId: "agree", event: "agree" });
      } else {
        resolve({ buttonId: "disagree", event: "disagree" });
      }
    }
  });
});
```

**关键点**：
- `resolve` 必须被调用，否则隐私 API 调用会永久挂起
- 使用 `wx.showModal` 展示协议（不触发隐私检查，避免死循环）
- 用户同意后，后续隐私 API 调用不再弹窗
- 用户不同意后，隐私 API 调用会直接失败

**参考**：[微信官方隐私保护指引](https://developers.weixin.qq.com/miniprogram/dev/framework/user-privacy/)


### 6.5 app.json JSON 尾随逗号

**现象**：编译报错 `miniprogram/app.json Expecting 'STRING', got }`。

**根因**：`app.json` 中最后一项属性末尾多了逗号，JSON 标准不允许尾随逗号。

```json
// 错误
{ "sitemapLocation": "sitemap.json", }

// 正确
{ "sitemapLocation": "sitemap.json" }
```

**预防**：使用 `python3 -c "import json; json.load(open('app.json'))"` 验证 JSON 合法性。

---

## 附录：常用排查命令

```bash
# 检查 Windows 后台是否监听 8000 端口
netstat -ano | findstr :8000

# 测试 Windows 后台响应
curl http://localhost:8000/api/register

# PowerShell 查看防火墙规则
Get-NetFirewallRule | Where-Object { $_.DisplayName -like "*8000*" }

# 查看 Windows 局域网 IP
ipconfig | findstr /i "IPv4"
```

---

> **维护说明**：每遇到一个新问题，在此文档追加一节（现象 → 根因 → 方案 → 预防）。保持格式统一便于检索。
