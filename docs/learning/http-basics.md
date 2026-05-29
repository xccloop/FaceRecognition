# HTTP 协议基础

## 在这个项目中的作用

HTTP 用于两个链路：手机 → Windows 后台（注册上传）和 Windows → 树莓派（特征同步）。理解 HTTP 的基本格式是看懂这两段通信的前提。

## 项目中用到的具体场景

### 场景 1：multipart/form-data 上传（手机 → Windows）

微信小程序用 `wx.uploadFile` 上传照片 + 姓名，HTTP 请求格式为：

```http
POST /api/register HTTP/1.1
Content-Type: multipart/form-data; boundary=----WebKitFormBoundary
Content-Length: 45230

------WebKitFormBoundary
Content-Disposition: form-data; name="name"

张三
------WebKitFormBoundary
Content-Disposition: form-data; name="photo"; filename="face.jpg"
Content-Type: image/jpeg

<JPEG 二进制数据>
------WebKitFormBoundary--
```

Windows 端用 FastAPI 的 `File` 和 `Form` 参数接收：

```python
@app.post("/api/register")
async def register(photo: UploadFile = File(...), name: str = Form(...)):
    ...
```

### 场景 2：HTTP POST 同步注册（Windows → Pi）

Windows 用 Python `requests` 库 POST 到树莓派的 Flask API：

```python
# Windows/sync.py
import requests
files = {"photo": ("face.jpg", file_bytes, "image/jpeg")}
data = {"name": "张三"}
resp = requests.post("http://192.168.137.100:5000/api/register",
                     files=files, data=data, timeout=timeout)
```

### 场景 3：MJPEG 推流（Pi 端 HTTP multipart 响应）

```http
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg

<JPEG frame 1>

--frame
Content-Type: image/jpeg

<JPEG frame 2>
...
```

这用的是 `multipart/x-mixed-replace`，浏览器收到新 part 就替换旧内容，形成连续视频效果。

### 场景 4：RESTful API 去重检查

```python
# 检查 Pi 上是否已有同名用户
resp = requests.get(f"http://{host}:5000/api/features?name={name}")
```

### 场景 5：HTTP DELETE 删除

```python
# 从 Pi 端删除已注册用户
resp = requests.delete(f"http://{host}:5000/api/features?name={name}")
```

## 核心概念（项目中涉及的部分）

| 概念 | 说明 | 项目中的体现 |
|------|------|-------------|
| HTTP Method | GET/POST/DELETE | GET 查特征、POST 注册、DELETE 删除 |
| Content-Type | 告诉服务器/客户端数据格式 | `multipart/form-data`、`image/jpeg`、`multipart/x-mixed-replace` |
| Status Code | 服务器响应状态 | 200 OK、4xx 客户端错、5xx 服务器错 |
| Request/Response | 一问一答模式 | 每次 HTTP 交互都是客户端发请求、服务器回响应 |
| Timeout | 等多久算超时 | `requests.post(..., timeout=15)` — 15 秒没回复就放弃 |
| URL 参数 | `?name=xxx` 传参数 | `GET /api/features?name=张三` |

## 与串口协议的对比

| 特性 | HTTP（Win↔Pi） | 自定义帧协议（Pi↔STM32） |
|------|---------------|------------------------|
| 传输层 | TCP（可靠） | UART（不可靠） |
| 数据格式 | 文本头 + 二进制体 | 全二进制 |
| 校验 | TCP 自带校验和 | 需要自定义 CRC8 |
| 帧定界 | Content-Length 头 | COBS + 0x00 分隔符 |
| 适用场景 | 网络环境、大数据量 | 串口、短帧、实时性 |

## 要学到什么程度

- 理解 HTTP 是请求-响应模型
- 知道 GET/POST/DELETE 三种方法的基本用途
- 理解 Content-Type 的作用
- 理解 multipart/form-data 适合上传文件
- 知道 timeout 机制的必要性
