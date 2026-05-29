# 树莓派 4B 开发流程

## 一、硬件认知

### 树莓派 4B 规格（2GB 版）
- CPU: Broadcom BCM2711, 四核 Cortex-A72 @ 1.5GHz（ARMv8 64位）
- 内存: 2GB LPDDR4
- 接口: 2×USB 3.0 + 2×USB 2.0, 千兆网口, 2×micro-HDMI, 3.5mm 音频
- 存储: microSD 卡（系统盘，建议 ≥32GB，Class 10/A1）
- GPIO: 40 针引脚
- 供电: USB-C, 5V/3A

### 与普通 PC 开发的关键区别
| 项目 | PC (x86) | 树莓派 (ARM) |
|------|----------|--------------|
| 架构 | x86_64 | aarch64 (ARMv8) |
| 性能 | 强 | 弱（编译大型库如 OpenCV 需 2-6 小时） |
| 编译方式 | 本地编译 | 本地编译 / 交叉编译 |
| 外设 | 无 GPIO | 有 GPIO、CSI 摄像头、DSI 屏幕 |

---

## 二、系统烧录与启动

### 2.1 下载工具和镜像
- 烧录工具: [Raspberry Pi Imager](https://www.raspberrypi.com/software/)（Windows/Mac/Linux 均有）
- 镜像选择: Raspberry Pi OS Lite (64-bit)，无桌面环境，省内存

### 2.2 烧录步骤
```
1. 电脑插入 microSD 卡（用读卡器）
2. 打开 Raspberry Pi Imager
3. 选择设备: Raspberry Pi 4
4. 选择系统: Raspberry Pi OS (other) → Raspberry Pi OS Lite (64-bit)
5. 选择存储: 你的 SD 卡
6. 点击齿轮图标 ⚙，预设配置:
   - 设置主机名: raspberrypi-fr（可选）
   - 启用 SSH（必须！选"使用密码登录"）
   - 设置用户名/密码: pi / 你的密码
   - 配置 WiFi: SSID + 密码（如果不用网线）
   - 语言时区: Asia/Shanghai
7. 点击写入，等待完成
```

### 2.3 首次启动
```
1. SD 卡插入树莓派
2. 连接电源（USB-C）
3. LED 红灯常亮（供电正常），绿灯闪烁（SD 卡读写）
4. 等待 30-60 秒启动完成
```

---

## 三、连接树莓派

### 3.1 通过路由器查找 IP
```powershell
# Windows PowerShell 中扫描局域网
arp -a | findstr "b8-27-eb"
arp -a | findstr "dc-a6-32"
arp -a | findstr "e4-5f-01"
# 树莓派 MAC 地址前缀通常为以上三种之一
```

### 3.2 SSH 连接
```powershell
# Windows 自带 ssh（Win10 1809+），或用 PuTTY / MobaXterm
ssh pi@192.168.x.x
# 默认密码: raspberry（或用 Imager 中自己设的密码）
```

### 3.3 首次登录后必做
```bash
# 更新系统
sudo apt update && sudo apt upgrade -y

# 开启 SSH 永久启用（如果烧录时忘开）
sudo systemctl enable ssh
sudo systemctl start ssh

# 修改密码（如果还是默认的 raspberry）
passwd

# 查看系统信息
uname -a          # 内核版本
cat /proc/cpuinfo # CPU 信息
free -h           # 内存使用
df -h             # 磁盘空间
```

---

## 四、三种开发方式对比

```
┌─────────────────────────────────────────────────────────────────┐
│                    树莓派开发三种方式                              │
├──────────────┬──────────────────┬────────────────────────────────┤
│   方式        │     怎么做        │          适用场景               │
├──────────────┼──────────────────┼────────────────────────────────┤
│ 1. 原生开发   │ 在 Pi 上写代码     │ 小项目、脚本、快速验证            │
│              │ 在 Pi 上编译      │ 优点是环境一致，不需要交叉工具链     │
│              │                   │ 缺点是编译慢，编辑器简陋            │
├──────────────┼──────────────────┼────────────────────────────────┤
│ 2. 远程开发   │ PC 写代码         │ ★ 推荐，本项目采用此方式            │
│   (推荐)     │ 代码传到 Pi       │ 用 VS Code Remote-SSH 直接在 Pi    │
│              │ Pi 上编译运行     │ 上编辑 + 编译，体验接近本地开发       │
├──────────────┼──────────────────┼────────────────────────────────┤
│ 3. 交叉编译   │ PC 写代码         │ 大型项目、CI/CD、编译速度敏感        │
│              │ PC 上交叉编译     │ 需要搭建 ARM 交叉编译工具链          │
│              │ 传二进制到 Pi     │ 复杂度高，不适合入门                 │
└──────────────┴──────────────────┴────────────────────────────────┘
```

### 方式一：直接在 Pi 上写代码（原生）
```bash
# 在 SSH 中用 nano/vim 编辑
nano main.cpp
# 编译
g++ main.cpp -o app
```

### 方式二（推荐）：VS Code Remote-SSH
```
PC 端:
  1. VS Code 安装 Remote - SSH 插件
  2. Ctrl+Shift+P → "Remote-SSH: Connect to Host"
  3. 输入: pi@192.168.x.x
  4. 第一次连接会下载 vscode-server 到 Pi 上
  5. 之后就可以和本地开发一样，直接在 VS Code 中编辑 Pi 上的文件
  6. VS Code 终端 = Pi 的终端，直接 make/build

优点:
  - PC 端代码编辑体验（语法高亮、智能提示、搜索）
  - 实际编译在 Pi 上执行，没有交叉编译兼容问题
  - 调试也可以用 GDB
```

### 方式三：交叉编译（高级）
```
流程: PC(x86) → ARM 交叉编译器 → ARM 二进制 → 传到 Pi → 运行

需要的工具:
  - ARM 交叉编译工具链: sudo apt install gcc-aarch64-linux-gnu
  - 需要把 ncnn、OpenCV 也交叉编译一遍（非常复杂）
  - 适合有经验的嵌入式开发者
```

---

## 五、本项目开发流程（方式二：远程开发）

### 5.1 整体流程图

```
Windows PC                        树莓派 4B
─────────                        ──────────
VS Code  ──SSH──►  写代码（源码在 Pi 上）
                    ↓
                 编译（cmake + make）
                    ↓
                 运行测试 ./face_recog
                    ↓
                 调试（GDB / printf）
                    ↓
                 完成 → 代码传回 PC 归档到 Git
```

### 5.2 具体步骤

**Step 1 — 树莓派环境准备（一次性）**
```bash
# 安装编译工具
sudo apt install -y build-essential cmake git

# 安装 OpenCV（推荐 apt 安装，不用自己编译）
sudo apt install -y libopencv-dev

# 编译安装 ncnn
cd ~
git clone https://github.com/Tencent/ncnn.git
cd ncnn
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DNCNN_BUILD_EXAMPLES=OFF ..
make -j4
sudo make install
sudo ldconfig  # 更新动态库缓存
```

**Step 2 — PC 端 VS Code 配置**
```
1. 安装 Remote-SSH 插件
2. 配置 ~/.ssh/config（可选）:
   Host rpi4
     HostName 192.168.x.x
     User pi
     Port 22
3. Ctrl+Shift+P → Remote-SSH: Connect to Host → rpi4
4. 打开 Pi 上的项目目录，开始写代码
```

**Step 3 — 代码开发（在 Pi 上）**
```bash
# 项目目录结构（在 Pi 的 ~/face_recognition 下）
face_recognition/
├── Model/
│   ├── scrfd.param
│   ├── scrfd.bin
│   ├── mbf.param
│   └── mbf.bin
├── src/
│   └── main.cpp
├── include/
│   └── (头文件)
└── CMakeLists.txt
```

**Step 4 — CMake + ncnn 配置**
```cmake
cmake_minimum_required(VERSION 3.10)
project(face_recognition)

set(CMAKE_CXX_STANDARD 11)

find_package(OpenCV REQUIRED)
find_package(ncnn REQUIRED)

add_executable(face_recog src/main.cpp)
target_link_libraries(face_recog ncnn ${OpenCV_LIBS})
```

**Step 5 — 编译和运行**
```bash
cd ~/face_recognition
mkdir build && cd build
cmake ..
make -j4
./face_recog
```

---

## 六、文件传输方式

如果需要从 PC 传文件到 Pi（或反向）：

```powershell
# 方式1: scp（Windows 自带）
scp D:\FaceRecognition\Linux\Model\*.param pi@192.168.x.x:~/face_recognition/Model/
scp pi@192.168.x.x:~/face_recognition/output.txt D:\FaceRecognition\Linux\

# 方式2: VS Code Remote-SSH 直接拖拽文件

# 方式3: rsync（Pi 端执行，需 PC 开共享或反过来）
# 方式4: Samba 共享文件夹，PC 和 Pi 都能访问
```

---

## 七、调试方法

### 7.1 GDB 命令行调试
```bash
# 编译时加 -g 生成调试符号
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j4

# 启动 GDB
gdb ./face_recog
(gdb) run image.jpg    # 传参运行
(gdb) bt               # 崩溃时看调用栈
(gdb) print var        # 查看变量
```

### 7.2 VS Code 图形化调试
```
在 Remote-SSH 模式下，F5 直接启动调试，
和本地 C++ 调试体验完全一致（断点、单步、变量查看等）
```

### 7.3 打印调试（最常用）
```cpp
#include <iostream>
// 在 ncnn 推理前后打印耗时
auto start = std::chrono::steady_clock::now();
// ... 推理 ...
auto end = std::chrono::steady_clock::now();
std::cout << "inference: " 
          << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() 
          << "ms" << std::endl;
```

---

## 八、性能优化常识

在树莓派 4B 2GB 上跑 ncnn:

| 优化项 | 方法 | 预期效果 |
|--------|------|----------|
| ncnn 开启 ARM NEON | cmake 时自动检测 | 2-4x 加速 |
| 多线程推理 | `ncnn::Net::opt num_threads = 4` | 利用 4 核 |
| 模型量化 (INT8) | 用 ncnn2table + ncnn2int8 工具 | 速度提升，精度略微下降 |
| OpenCV 用 NEON | `apt install libopencv-dev` 自动包含 | 图像处理加速 |
| 降低输入分辨率 | 检测前缩小图像 | 大幅提速 |

---

## 九、针对本项目的建议流程

```
现在（Pi 没到）:
  ├── 1. 在 Windows 上用 VS Code + CMake 把代码框架写好
  │      （不涉及 ncnn/OpenCV 调用的部分可以先写逻辑）
  ├── 2. 下载好模型文件到 Model/ 目录
  └── 3. 在 PC 上装个 Ubuntu 虚拟机或用 WSL2 模拟 Linux 环境测试代码

Pi 到手后:
  ├── 4. 烧录系统、开机、联网
  ├── 5. 安装 ncnn + OpenCV
  ├── 6. VS Code Remote-SSH 连接
  ├── 7. 把代码和模型传到 Pi 上
  ├── 8. cmake + make 编译
  └── 9. 运行、调试、调优
```

---

## 十、常见问题 FAQ

**Q: 编译 ncnn 时报错 "NEON not supported"**
```
A: 确认 Pi 用的是 64 位系统（aarch64）。
   32 位系统需要额外处理 NEON 编译选项。
```

**Q: make -j4 时卡死或报错**
```
A: 2GB 内存编译大项目容易 OOM。改用 make -j1 或 make -j2，
   或者增加 swap: sudo dphys-swapfile swapoff && 
   sudo sed -i 's/CONF_SWAPSIZE=.*/CONF_SWAPSIZE=2048/' /etc/dphys-swapfile && 
   sudo dphys-swapfile swapon
```

**Q: 怎么固定 Pi 的 IP 地址？**
```
A: 在路由器里绑定 MAC-IP，或者 Pi 上编辑 /etc/dhcpcd.conf:
   interface wlan0
   static ip_address=192.168.1.100/24
   static routers=192.168.1.1
   static domain_name_servers=192.168.1.1
```

**Q: 烧录的 SD 卡 Pi 不启动？**
```
A: 检查: ① 电源是否 5V/3A（手机充电头可能不够）
         ② SD 卡接触是否良好
         ③ 绿灯是否闪烁（不闪 = 没读到系统）
         ④ 重新用 Imager 烧录一次
```

**Q: PC 写好的 x86 程序能直接在 Pi 上跑吗？**
```
A: 不能。x86 和 ARM 是不同架构，必须重新编译。
   要么在 Pi 上编译（方式一/二），要么在 PC 交叉编译（方式三）。
```
