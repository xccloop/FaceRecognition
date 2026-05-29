# UART 串口

## 在这个项目中的作用

UART 是树莓派和 STM32 之间的物理通信链路。识别结果（IDENTIFY/UNKNOWN/NOFACE/MULTIFACE）和心跳信号都通过 3 根线（TX、RX、GND）传输。

对应代码：
- 树莓派端：`Linux/Model/src/main.cpp` — `SerialPort` 类（第 169-250 行）
- STM32 端：`Stm32/Comms/uart_dma.c`、`Stm32/Comms/uart_port.c`

## 项目中用到的具体知识

### 物理层参数

```cpp
// Pi 端 — POSIX termios 配置
baud = 115200;              // 波特率
8N1: 8 数据位 / 无校验位 (PARENB=0) / 1 停止位 (CSTOPB=0)
```

| 参数 | 本项目的值 | 含义 |
|------|-----------|------|
| 波特率 | 115200 bps | 每秒传输 115200 位 ≈ 14.4 KB/s |
| 数据位 | 8 | 每个字节 8 位有效数据 |
| 校验位 | 无 (N) | 不额外发校验位（帧层有 CRC8） |
| 停止位 | 1 | 每个字节后 1 个停止位 |

### Pi 端：POSIX termios 配置

```cpp
// main.cpp — SerialPort::open()
struct termios tty;

// 控制标志
tty.c_cflag |= (CLOCAL | CREAD);    // 忽略调制解调器、启用接收
tty.c_cflag &= ~CSIZE;              // 清除数据位配置
tty.c_cflag |= CS8;                 // 8 数据位
tty.c_cflag &= ~PARENB;             // 无校验
tty.c_cflag &= ~CSTOPB;             // 1 停止位
tty.c_cflag &= ~CRTSCTS;            // 无硬件流控

// 本地标志：关闭终端处理（raw mode）
tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

// 输入标志：关闭软件流控
tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);

// 输出标志：raw mode
tty.c_oflag &= ~OPOST;

// 超时配置
tty.c_cc[VMIN] = 0;   // 不等待最小字节数
tty.c_cc[VTIME] = 1;  // 100ms 超时（0.1 秒 × 10）

tcsetattr(fd_, TCSANOW, &tty);
```

关键理解：**Raw mode** — 关闭所有终端处理（换行转换、回显、信号处理），把串口当纯数据通道用。

### STM32 端：非阻塞 DMA

STM32 端不开接收中断（RXNE），数据完全由 DMA 搬运到 ring buffer。CPU 只在 IDLE 中断时介入——效率最高。

### TTL 电平

树莓派的 GPIO 串口和 STM32 的 USART 都是 TTL 电平（3.3V），可以直接用杜邦线连接，不需要 MAX232 之类的电平转换芯片。接线方式：

```
Pi TX  ──── STM32 RX
Pi RX  ──── STM32 TX
Pi GND ──── STM32 GND
```

### tcdrain — 等待发送完成

```cpp
// Pi 端 write 函数
ssize_t n = ::write(fd_, data, len);
tcdrain(fd_);  // 阻塞直到所有数据从内核缓冲区发送完毕
```

确保 write 返回后数据已经发出，而不是还留在内核缓冲区。

## 要学到什么程度

- 理解波特率的含义：每秒传输的符号数（UART 中 1 符号 = 1 bit = 1 波特）
- 理解 8N1 各字段的含义
- 理解 raw mode vs cooked mode：raw mode 不做任何终端转换
- 知道 `tcdrain` 和 `tcflush` 的区别
