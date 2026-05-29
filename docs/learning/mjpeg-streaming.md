# MJPEG 推流

## 在这个项目中的作用

树莓派上的 MJPEG 推流功能让 Windows 管理面板能通过浏览器看到摄像头实时画面。它是一种最简单的视频流方案：每帧 JPEG 图片通过 HTTP 不断发送。

对应代码：`Linux/Model/src/main.cpp` — `MJPEGServer` 类（第 38-164 行）

## 核心原理

### HTTP multipart 响应

MJPEG 利用 HTTP 的 `multipart/x-mixed-replace` 特性——服务端不断发送新的图片帧，浏览器收到新帧就替换旧帧，形成视频效果：

```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg

<JPEG bytes>

--frame
Content-Type: image/jpeg

<JPEG bytes>
...
```

### 项目中不用 HTTP 库，手写 raw socket

为了保持单进程、零依赖，MJPEG 服务器用原生 socket API 实现：

```cpp
// main.cpp — MJPEGServer::serve()
int server = socket(AF_INET, SOCK_STREAM, 0);     // 创建 TCP socket
bind(server, ...);                                  // 绑定 8080 端口
listen(server, 5);                                  // 开始监听

// select 轮询等待连接（设置 1s 超时，以便能响应 running_ 标志位）
fd_set fds;
struct timeval tv = {1, 0};
select(server + 1, &fds, nullptr, nullptr, &tv);

int client = accept(server, nullptr, nullptr);      // 接受连接
```

### TCP_NODELAY — 关键性能优化

```cpp
int on = 1;
setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
```

默认 TCP 会缓冲数据等凑满一个包再发（Nagle 算法），对实时视频流是灾难。`TCP_NODELAY` 关闭这个延迟，让每帧 JPEG 立即发送。

### 帧发送逻辑

```cpp
// 每 40ms 检查一次是否有新帧（~25fps 上限）
auto serveClient = [&](int fd) {
    while (running_) {
        std::vector<uchar> frame;
        { std::lock_guard<std::mutex> lock(mtx_); frame = latestJpeg_; }

        if (!frame.empty() && frame != lastSent) {
            send(fd, "--frame\r\nContent-Type: image/jpeg\r\n\r\n", ..., MSG_NOSIGNAL);
            send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            send(fd, "\r\n", 2, MSG_NOSIGNAL);
            lastSent = frame;
        }
        usleep(40000);  // 40ms
    }
};
```

- `MSG_NOSIGNAL`：防止向已关闭连接发送数据时产生 SIGPIPE 信号导致进程崩溃
- `frame != lastSent`：只在帧内容变化时才发送，减少不必要传输

### 主线程如何喂帧

```cpp
// 把摄像头画面编码为 JPEG，质量 30（省带宽）
cv::Mat display;
cv::resize(frame, display, cv::Size(640, 480));
std::vector<uchar> jpg;
cv::imencode(".jpg", display, jpg, {cv::IMWRITE_JPEG_QUALITY, 30});
mjpeg.updateFrame(jpg);  // 更新 shared buffer（mutex 保护）
```

## 要学到什么程度

- 理解 MJPEG 的原理：HTTP multipart 持续推送 JPEG 帧
- 理解 TCP_NODELAY 对实时流的意义
- 知道 raw socket 编程的基本流程：socket → bind → listen → accept → send/recv
- 理解 `select()` 的作用：同时监听多个 socket 事件，带超时
