# Linux 基础

## 在这个项目中的作用

树莓派 4B 运行 Raspberry Pi OS（基于 Debian），所有 C++ 推理代码在无桌面的命令行环境下运行。你需要能脱离图形界面独立操作 Pi。

## 项目中用到的具体知识点

### 文件系统操作

C++ 代码中通过 `<filesystem>` 操作文件：

```cpp
// Linux/Model/src/main.cpp — FeatureDB 类
fs::create_directories("features");                          // 创建目录
fs::exists("features/" + name + ".bin");                     // 判断文件是否存在
fs::directory_iterator("features")                           // 遍历目录
```

### 进程信息读取

运行中读取 CPU 温度和进程 CPU 时间（Linux 特有的 `/proc` 和 `/sys` 伪文件系统）：

```cpp
// 读取 CPU 温度
std::ifstream tf("/sys/class/thermal/thermal_zone0/temp");   // 树莓派温度传感器

// 读取进程 CPU 时间
std::ifstream sf("/proc/self/stat");                         // 当前进程统计信息
```

- **`/sys/class/thermal/thermal_zone0/temp`**：树莓派 CPU 温度，单位毫摄氏度
- **`/proc/self/stat`**：当前进程的 utime（用户态时间）、stime（内核态时间），用于计算 CPU 占用率

### Shell 脚本

项目启动脚本 `start.sh` 负责启动 C++ 推理程序和 Python 辅助服务。

### 串口设备文件

Linux 下串口是设备文件，项目中用 `/dev/serial0`：

```cpp
uart.open("/dev/serial0", 115200);
```

## 要学到什么程度

- 能在无桌面的 Pi 上通过命令行完成所有操作（文件管理、进程管理、编辑文件）
- 理解 `/proc` 和 `/sys` 的作用——它们是 Linux 内核向用户空间暴露信息的接口
- 理解设备文件的概念（`/dev/serial0`、`/dev/video0`）——一切皆文件
- 会写简单的 Shell 脚本来启动/停止服务
