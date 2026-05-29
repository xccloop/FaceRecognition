# 多线程

## 在这个项目中的作用

树莓派 C++ 程序需要同时处理多条流水线：摄像头采集、ncnn 推理、MJPEG 推流、UART 收发、特征库热加载。如果不使用多线程，任何一个环节的阻塞都会拖慢全局。

对应代码：`Linux/Model/src/main.cpp`

## 项目中用到的具体知识

### 1. std::thread — 独立线程

MJPEG 服务器运行在独立线程中，与主推理循环并行：

```cpp
// main.cpp — MJPEGServer
class MJPEGServer {
    std::thread thread_;
    std::atomic<bool> running_{false};

    void start(int port = 8080) {
        running_ = true;
        thread_ = std::thread(&MJPEGServer::serve, this);  // 启动线程
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();  // 等待线程退出
    }
};
```

### 2. std::mutex — 互斥锁

两个线程共享同一份数据时，必须加锁：

```cpp
// MJPEGServer — 主线程写，推流线程读
std::mutex mtx_;
std::vector<uchar> latestJpeg_;

void updateFrame(const std::vector<uchar>& jpg) {
    std::lock_guard<std::mutex> lock(mtx_);  // 自动加锁/解锁
    latestJpeg_ = jpg;
}

// serve() 中读取：
std::lock_guard<std::mutex> lock(mtx_);
frame = latestJpeg_;
```

### 3. FeatureDB 的并发保护

特征库被主线程（推理+比对）和热加载（每 5s 从磁盘重新读取）共享：

```cpp
// main.cpp — FeatureDB
class FeatureDB {
    mutable std::mutex mtx_;   // mutable 允许在 const 函数中加锁

    Match identify(const std::vector<float>& feat) const {
        std::lock_guard<std::mutex> lock(mtx_);  // 读也加锁
        // ... 遍历匹配
    }

    void reload() {
        std::lock_guard<std::mutex> lock(mtx_);  // 写也加锁
        // ... 重新加载
    }
};
```

### 4. std::atomic — 原子变量

用于简单的标志位，不需要加锁：

```cpp
std::atomic<bool> running{true};   // 多线程安全的 bool
```

原子变量保证对它的读写是不可分割的原始操作，不会被其他线程"插队"。

### 5. 线程间通信：MJPEG 停止技巧

停止 MJPEG 线程时，主线程通过 `connect()` 自己来解除 `accept()` 的阻塞：

```cpp
void stop() {
    running_ = false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    // connect 到自己的端口，使 accept() 返回
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    close(fd);
    if (thread_.joinable()) thread_.join();
}
```

这是常见技巧：`accept()` 是阻塞的，直接 `join()` 会永远等不到。通过发送一个连接请求"唤醒"它。

## 要学到什么程度

- 理解线程的概念：一个进程可以同时执行多个任务
- 理解互斥锁（mutex）的作用：保护共享数据不被同时读写
- 理解 RAII 风格加锁（`lock_guard`）：构造时加锁，析构时解锁，不依赖手动 `unlock()`
- 理解 `std::atomic` 和 mutex 的区别：atomic 适合简单标志位，mutex 适合保护复杂数据结构
- 知道竞态条件（race condition）的后果：数据错乱、崩溃
