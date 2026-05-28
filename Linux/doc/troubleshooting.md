# 树莓派端问题排查与解决记录

> FaceRecognition 项目 — 树莓派 4B 部署过程中遇到的所有问题及解决方案

---

## 1. ncnn 编译问题

### 1.1 GitHub 无法访问（网络问题）

**现象**：`git clone https://github.com/Tencent/ncnn.git` 超时，`Failed to connect to github.com port 443`

**原因**：树莓派所在网络环境 GitHub 不可达。

**解决**：在能访问 GitHub 的机器上下载 ncnn 源码 zip 包，通过 SFTP (`paramiko`) 推送到 Pi 上解压编译。

```bash
# 在可上网的机器上
wget https://github.com/Tencent/ncnn/archive/refs/tags/20241226.zip
# 或直接 git clone --branch 20241226

# 推到 Pi
scp ncnn-20241226.zip qxc@192.168.137.100:/home/qxc/
# Pi 上解压
unzip ncnn-20241226.zip && mv ncnn-20241226 ncnn_build
```

### 1.2 编译时内存不足

**现象**：`make -j4` 导致 OOM，进程被 kill。

**原因**：Pi 4B 只有 1.8GB 内存，ncnn 源码编译时需要大量内存。

**解决**：使用 `make -j2`（2 个并行编译线程）。

```bash
cd ncnn_build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DNCNN_BUILD_TOOLS=OFF \
      -DNCNN_BUILD_EXAMPLES=OFF \
      -DNCNN_BUILD_BENCHMARK=OFF \
      -DNCNN_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      ..
make -j2
sudo make install
sudo ldconfig
```

### 1.3 OpenMP 链接错误

**现象**：编译 C++ 项目时报 `undefined reference to symbol 'omp_set_dynamic@@OMP_1.0'`

**原因**：ncnn 依赖 OpenMP，但 CMakeLists.txt 没有显式链接它。

**解决**：在 CMakeLists.txt 添加 `find_package(OpenMP REQUIRED)` 并链接 `OpenMP::OpenMP_CXX`。

### 1.4 camke 缓存路径失效

**现象**：`make: *** No rule to make target ...` 或 cmake 报源目录不存在。

**原因**：整个项目目录被移动（`~/Desktop/Model` → `~/Desktop/FaceRec/Model`），build 目录中的 CMakeCache.txt 记录了旧路径。

**解决**：
```bash
cd build && rm -f CMakeCache.txt && cmake -DNCNN_DIR=/usr/local ..
make -j2
```

---

## 2. 摄像头问题

### 2.1 usb摄像头设备繁忙 (Device busy)

**现象**：OpenCV 报 `Device '/dev/video0' is busy`，摄像头无法打开。

**原因**：两个进程同时试图独占 `/dev/video0`——例如 Python MJPEG 和 C++ facerec 同时运行时，先启动的占用了摄像头。

**解决**：将 MJPEG 服务器集成到 C++ 进程中（内建 HTTP 服务），只由一个进程访问摄像头。

### 2.2 空帧导致进程退出

**现象**：程序运行一段时间后自动退出，日志显示 `Camera empty frame`。

**原因**：USB 摄像头偶尔返回空帧，代码中 `if (frame.empty()) break;` 会导致主循环退出。

**解决**：空帧时不退出，而是自动重连摄像头：
```cpp
if (frame.empty()) {
    cap.release();
    cap.open(0);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
    continue;
}
```

### 2.3 640x480 分辨率导致帧率骤降

**现象**：将摄像头设为 640x480 后，FPS 从 30 降到 12。

**原因**：4 倍像素数意味着更长的采集和编码时间。

**解决**：摄像头保持 320x240 采集（推理快），JPEG 写入前用 `cv::resize` 放大到 640x480 给 MJPEG 推流。

---

## 3. UART 串口问题

### 3.1 /dev/serial0 不存在

**现象**：`ls /dev/serial0` 返回 `No such file or directory`。

**原因**：Pi 4 的硬件 UART (ttyAMA0) 默认被蓝牙占用。

**解决**：编辑 `/boot/firmware/config.txt`，添加两行并重启：
```
enable_uart=1
dtoverlay=disable-bt
```
重启后 `/dev/serial0` → `/dev/ttyAMA0` 会自动出现。

### 3.2 重启后 UART 失效

**现象**：每次重启 UART 配置似乎不生效。

**原因**：可能没有保存 config.txt 修改，或者使用了错误的路径（`/boot/config.txt` vs `/boot/firmware/config.txt`，Pi 4/5 用后者）。

**解决**：确认编辑的是 `/boot/firmware/config.txt`，因为 Pi 4B + Bookworm 使用此路径。

---

## 4. CPU 温度与散热

### 4.1 芯片温度过高 (80°C+)

**现象**：`vcgencmd measure_temp` 显示 80°C+，芯片烫手。

**原因**：ncnn 推理使用 4 线程占满所有核心，CPU 持续满载。

**解决**：
1. **软件降负载**：ncnn 线程数从 4 降到 2（`net_.opt.num_threads = 2`），CPU 从 ~300% 降到 ~190%
2. **硬件散热**：贴散热片 + 接 5V 风扇，温度从 61°C 降到 43°C

### 4.2 温度正常但仍担心长期运行安全

**现实检查**：Pi 4 的热保护降频线是 80°C。在 43-45°C (有风扇) 或 61°C (无风扇) 下 7×24 运行完全安全。芯片设计允许 100°C+ 才会损坏。

---

## 5. 进程生命周期

### 5.1 SSH 断开后进程退出

**现象**：通过 SSH 手动运行 `./start.sh`，关掉终端后程序退出。

**原因**：SSH 会话断开时，终端发送 SIGHUP 信号给前台进程组，导致程序退出。

**解决**：
1. **推荐**：使用 systemd 服务（`sudo systemctl start face-recognition`），与 SSH 无关
2. **临时**：`nohup ./User/start.sh > /tmp/log 2>&1 &`

### 5.2 systemd 服务频繁重启

**现象**：`systemctl status face-recognition` 显示 `Restart counter is at 65`。

**原因**：程序中未处理的异常或空帧退出导致进程终止，systemd 的 `Restart=on-failure` 触发重启。

**解决**：给主循环加 `try/catch` 捕获所有异常并自动恢复，空帧改为重连摄像头而非退出。

### 5.3 重定向日志后看不到输出

**现象**：`journalctl -u face-recognition` 看不到推理日志，只有启动横幅。

**原因**：start.sh 用 `nohup ... > /tmp/log 2>&1 &` 把输出重定向到了文件。

**解决**：改用 `exec "$BINARY" run`，让日志直接进 systemd journal。`journalctl -u face-recognition -f` 实时查看。

---

## 6. MJPEG 推流问题

### 6.1 画面卡死/定格

**现象**：浏览器打开 MJPEG 流后画面定格在一帧不动。

**原因**：
- 使用 `select()` + writable 检查可能导致帧丢失
- 在 tmpfs (/dev/shm) 上使用 `mtime` 缓存可能不可靠
- 推理进程停止后帧文件不再更新

**解决**：
1. 最终方案：将 MJPEG 服务器集成到 C++ 进程中（内存直传，不走文件）
2. 中间方案：简化 Python MJPEG 为每线程独立轮询，去掉 writable 检查

### 6.2 画面卡顿

**现象**：画面断续，不流畅。

**原因**：TCP Nagle 算法延迟（等待缓冲区填满才发送）、JPEG 质量过高导致传输阻塞。

**解决**：
1. `TCP_NODELAY` — 禁用 Nagle 算法（`conn.setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)`）
2. JPEG 质量从 50 降到 30 — 文件从 7KB 降到 4KB
3. 每帧都写 JPEG（之前每 2 帧写一次）
4. MJPEG 帧率从 15 提到 25fps

### 6.3 Windows 客户端连不上

**现象**：浏览器能看 MJPEG，但 Windows exe 显示"未连接"。

**原因**：
1. 旧 `config.json` 覆盖了新默认值（IP 还是旧值）
2. 视频路径 `/stream` vs `/video` 不匹配
3. MJPEG 进程没在跑

**解决**：
1. 删掉 exe 目录下的 `config.json` 或在设置页面更新 IP
2. 将路径统一为 `/video`
3. 确保 facerec 进程在运行（MJPEG 已内建）

---

## 7. 性能优化记录

| 改动 | 效果 |
|------|------|
| ncnn 线程 4→2 | CPU 300%→190%，温度 ↓ |
| 跳帧检测 `skip=3` | 推理负载 ↓ |
| 无人脸时仅每 2 帧检测 | FPS 21→30 |
| JPEG 质量 50→30 | 文件 7KB→4KB |
| MJPEG 内建到 C++ | 消除进程间竞争和文件 I/O |
| 风扇 + 散热片 | 温度 61°C→43°C |

---

## 8. 诊断命令速查

```bash
# 温度
vcgencmd measure_temp

# 降频状态（0x0=正常）
vcgencmd get_throttled

# 推理日志
journalctl -u face-recognition -f

# 串口测试
python3 ~/Desktop/uart_simulator.py

# 端口检查
ss -tlnp | grep -E "8080|5000"

# 摄像头测试
python3 -c "import cv2; c=cv2.VideoCapture(0); print(c.isOpened())"

# 手动重启
sudo systemctl restart face-recognition
```
