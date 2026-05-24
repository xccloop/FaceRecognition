# 树莓派 C++/ncnn 部署指南

> 将人脸识别引擎编译为 C++/ncnn 原生程序部署到树莓派，配合 Windows 管理端远程注册，上电自启。

---

## 架构概览

```
┌─────────────────────┐     HTTP (注册照片+姓名)      ┌──────────────────────┐
│   Windows 管理端     │ ──────────────────────────→  │   树莓派 4B           │
│   FaceRecognition.exe│                              │                       │
│   端口 8000          │ ←── MJPEG 视频流 ─────────── │  ┌─────────────────┐  │
│   (人员管理+配置)    │     http://pi:8080/video      │  │ facerec (C++)   │  │
└─────────────────────┘                              │  │ · 摄像头采集    │  │
                                                     │  │ · SCRFD 检测    │  │
                                                     │  │ · 特征提取+L2   │  │
                                                     │  │ · 特征库匹配    │  │
                                                     │  └───────┬─────────┘  │
                                                     │          │ UART       │
                                                     │  ┌───────▼─────────┐  │
                                                     │  │ pi_server.py    │  │
                                                     │  │ (Flask :5000)   │  │
                                                     │  │ 接收注册请求    │  │
                                                     │  │ 调 facerec CLI  │  │
                                                     │  └─────────────────┘  │
                                                     └──────────┬───────────┘
                                                                │ 串口 115200
                                                     ┌──────────▼───────────┐
                                                     │  STM32F103 (FreeRTOS)│
                                                     │  继电器/门锁/LED      │
                                                     └──────────────────────┘
```

**两个进程**：
- `facerec` (C++) — 实时识别主循环（摄像头 → 检测 → 识别 → UART）
- `pi_server.py` (Python/Flask) — HTTP API，接收 Windows 端注册请求，调 `facerec register` CLI

---

## 一、在树莓派上编译 C++ 程序

### 1.1 安装依赖

```bash
# 系统依赖
sudo apt update
sudo apt install -y build-essential cmake git \
    libopencv-dev libatlas-base-dev

# 编译 ncnn（ARM NEON 优化）
cd ~
git clone https://github.com/Tencent/ncnn.git
cd ncnn && git submodule update --init
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DNCNN_VULKAN=OFF \
      -DNCNN_BUILD_EXAMPLES=OFF \
      -DNCNN_BUILD_TOOLS=OFF ..
make -j4
sudo make install
sudo ldconfig
```

### 1.2 传输代码到树莓派

```powershell
# 从 Windows PowerShell
scp -r D:\FaceRecognition\Linux\Model pi@<树莓派IP>:~/facerec/
```

### 1.3 编译

```bash
cd ~/facerec/Model
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

编译产物：`build/facerec`（ARM 可执行文件，非 .exe）。

### 1.4 放置模型文件

```bash
mkdir -p ~/facerec/Model/models/ncnn_models
# 从 Windows 传输 ncnn 模型
# scp D:\FaceRecognition\Linux\Model\models\ncnn_models\*.param pi@<IP>:~/facerec/Model/models/ncnn_models/
# scp D:\FaceRecognition\Linux\Model\models\ncnn_models\*.bin   pi@<IP>:~/facerec/Model/models/ncnn_models/
```

模型文件清单：
- `det_500m_dyn.param` + `det_500m_dyn.bin`（SCRFD 检测，动态输入）
- `w600k_mbf.param` + `w600k_mbf.bin`（MobileFaceNet 特征提取）

### 1.5 验证

```bash
cd ~/facerec/Model/build
./facerec test test.jpg    # 测试人脸检测
./facerec live             # 启动实时识别
```

---

## 二、远程注册：Windows → 树莓派

### 2.1 注册流程

```
Windows 管理端                     树莓派
    │                                │
    │  POST /api/register            │
    │  multipart: photo + name       │
    │ ─────────────────────────────→ │ pi_server.py (:5000)
    │                                │   ↓ 保存照片到临时文件
    │                                │   ↓ 调用 facerec register
    │                                │   ↓ 特征存入 features/
    │  ← JSON {success, face_score}  │
    │                                │
```

### 2.2 在树莓派上启动注册 API 服务

`pi_server.py` 需要改为调用 C++ 的 `facerec register` 命令。修改后的版本见下文。

```bash
# 安装 Python 依赖
pip install flask numpy opencv-python

# 启动注册 API（端口 5000）
cd ~/facerec/Model
python pi_server.py --port 5000
```

### 2.3 Windows 端配置

在 Windows 管理端的 `config.json` 中设置：

```json
{
    "pi_host": "192.168.1.100",
    "pi_api_port": 5000
}
```

然后在人员管理页面注册人脸，系统自动同步到树莓派。

---

## 三、开机自启（systemd）

### 3.1 主识别服务（facerec）

```ini
# /etc/systemd/system/face-recog.service
[Unit]
Description=Face Recognition Engine (C++/ncnn)
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/facerec/Model/build
ExecStart=/home/pi/facerec/Model/build/facerec live
Restart=on-failure
RestartSec=5
StandardOutput=append:/var/log/face-recog.log
StandardError=append:/var/log/face-recog.log

[Install]
WantedBy=multi-user.target
```

### 3.2 注册 API 服务（pi_server.py）

```ini
# /etc/systemd/system/face-register.service
[Unit]
Description=Face Registration API (Flask)
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/facerec/Model
ExecStart=/usr/bin/python3 /home/pi/facerec/Model/pi_server.py --port 5000
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 3.3 安装服务

```bash
sudo cp face-recog.service /etc/systemd/system/
sudo cp face-register.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable face-recog face-register
sudo systemctl start face-recog face-register
```

### 3.4 查看状态

```bash
sudo systemctl status face-recog face-register
tail -f /var/log/face-recog.log
```

---

## 四、完整部署步骤（从零开始）

### 步骤 1：烧录系统
用 Raspberry Pi Imager 烧录 **Raspberry Pi OS Lite (64-bit)**，预配置 SSH + WiFi。

### 步骤 2：首次连接
```bash
ssh pi@<IP>
sudo apt update && sudo apt upgrade -y
```

### 步骤 3：安装编译环境（§1.1）
编译 ncnn + 系统依赖，约 15 分钟。

### 步骤 4：传输代码和模型（§1.2、§1.4）
从 Windows 用 `scp` 传输。

### 步骤 5：编译（§1.3）
```bash
cd ~/facerec/Model/build && cmake .. && make -j4
```

### 步骤 6：首次注册人脸
在 Windows 管理端打开 → 人员管理 → 注册 → 拍照 → 同步到 Pi。

### 步骤 7：安装开机自启（§3）
```bash
sudo systemctl enable face-recog face-register
sudo reboot
```

### 步骤 8：验证
```bash
# 检查服务
sudo systemctl status face-recog

# 看实时日志
tail -f /var/log/face-recog.log

# 看 MJPEG 视频流
# 浏览器打开 http://<PI_IP>:8080/video
```

---

## 五、目录结构（树莓派上）

```
/home/pi/facerec/
├── Model/
│   ├── build/
│   │   └── facerec              ← C++ 编译产物
│   ├── models/
│   │   └── ncnn_models/
│   │       ├── det_500m_dyn.param / .bin
│   │       └── w600k_mbf.param / .bin
│   ├── features/                ← 注册的人脸特征 (.bin)
│   │   ├── 张三.bin
│   │   └── 李四.bin
│   └── pi_server.py             ← 注册 API 服务
│
├── scripts/
│   ├── face-recog.service       ← 主识别 systemd 服务
│   └── face-register.service    ← 注册 API systemd 服务
│
└── log/
    └── face-recog.log
```

---

## 六、常见问题

**Q: 树莓派编译很慢？**
A: ncnn 编译约 10 分钟，Model 编译约 1 分钟。或使用交叉编译（在 Windows 上用 ARM 工具链编译后传输）。

**Q: facerec 报 "cannot open camera"？**
```bash
ls /dev/video*          # 查看摄像头设备
sudo chmod 666 /dev/video0
```

**Q: 串口不通？**
```bash
sudo raspi-config → Interface → Serial → 禁用 login shell, 启用硬件串口
ls /dev/serial0         # 应指向 /dev/ttyAMA0
```

**Q: 特征库在哪里？**
A: `~/facerec/Model/build/features/`（`facerec` 的工作目录）。Windows 远程注册的特征也保存在此。

**Q: 如何在不连 SSH 的情况下知道 Pi 的 IP？**
A: 路由器 DHCP 分配固定 IP，或在 Windows 管理端设置中配置 `pi_host`（支持 `.local` 域名如 `raspberrypi.local`）。
