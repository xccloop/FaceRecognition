# 串口可靠性

## 在这个项目中的作用

串口通信在物理层是脆弱的——可能遇上字节丢失、电气干扰、缓冲区溢出、帧不同步等问题。本项目从多个层面做了可靠性加固。

对应代码：
- `Stm32/Comms/uart_dma.c` — DMA ORE 恢复、IDLE 帧检测
- `Stm32/Protocol/frame_protocol.c` — CRC 校验、帧超时、缓冲区溢出保护
- `Stm32/User/main.c` — IWDG、心跳超时检测
- `Linux/Model/src/main.cpp` — `SerialPort` RAII、摄像头断线恢复

## 故障模式与应对

### 故障 1：ORE（Overrun Error）— 接收溢出

**原因**：CPU 没及时处理 DMA 搬到 ring buffer 的数据，新数据又来了。

**应对**（`uart_dma.c:on_error()`）：

```c
static void on_error(uint8_t p, uint32_t e) {
    h->error_count++;         // 记录，可查询
    h->error_restarts++;      // 记录重启次数
    rb_flush(&h->rx_rb);      // 错误期间的数据不可靠，清空
    h->rx_prev_cndtr = h->rx_rb.capacity;
    uart_port_dma_rx_stop(p);
    uart_port_dma_rx_start(p, ...);  // 重启 DMA 接收
}
```

关键设计决策：放弃错误窗口内的数据（可能有残留的好数据），但保证后续接收是干净的。宁可丢一帧，不能让错误扩散。

### 故障 2：字节间超时 — 半帧

**原因**：发送端崩溃、线缆松动、电气干扰导致传输中断。

**应对**（`frame_protocol.c:frame_parser_check_timeout()`）：

```c
// 5ms 字节间超时 — 由 FreeRTOS 5ms 定时器周期性检查
if (current_tick - g_last_tick >= 5) {
    g_state = STATE_WAIT_DELIMITER;  // 丢弃半帧，重置状态机
    g_buf_count = 0;
}
```

设计依据：115200 bps → 每字节 ~87μs，5ms 约 57 个字节的空闲 = 几乎可以确定帧传输中止。

### 故障 3：CRC 错误 — 比特翻转

**原因**：电气噪声导致线路上 1→0 或 0→1。

**应对**：CRC8 校验拒绝损坏帧（已在 `frame_protocol.c` 中实现），上层协议重发（Pi 端的心跳/周期性状态上报提供了天然重发机制）。

### 故障 4：缓冲区溢出 — 超长帧

**原因**：对端发了一个异常长的帧，或帧定界失败导致解析器一直收集字节。

**应对**：

```c
// frame_protocol.c
if (g_buf_count >= MAX_COBS_LEN) {
    g_state = STATE_WAIT_DELIMITER;  // 丢弃
    g_buf_count = 0;
}
```

### 故障 5：Pi 端心跳丢失 — 通信中断

**原因**：Pi 端死机、串口线断开、Pi 被重启。

**应对**（`main.c:vHeartbeatTimerCallback()`）：

```c
// 60 秒没收到任何帧 → 判断通信中断
if (xTaskGetTickCount() - xLastFrameTicks > 60000) {
    frame_parser_reset();        // 重置解析器
    uart_dma_rx_flush(hUart);    // 清空缓冲
    display_show_failure(REASON_TIMEOUT);  // 显示异常
    led_timeout_start();         // LED 闪烁告警
}
```

当通信恢复（收到帧），自动清除告警状态。这是项目的自愈设计——不依赖人工干预。

### 故障 6：Pi 端摄像头断线

**原因**：USB 摄像头松动、驱动异常。

**应对**（`main.cpp:cmdRun()`）：

```cpp
if (frame.empty()) {
    cap.release();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cap.open(0);  // 重新打开摄像头
    continue;
}
```

### 故障 7：异常捕获 — 全局兜底

```cpp
// main.cpp — 主循环 try-catch
try {
    // ... 完整推理管线 ...
} catch (const std::exception& e) {
    std::cerr << "[Run] Error: " << e.what() << " — recovering..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
```

任何未预期的异常（ncnn 推理失败、OpenCV 处理异常）不会让进程崩溃，而是打印错误后继续下一帧。

## 可靠性设计原则总结

| 原则 | 体现 |
|------|------|
| 故障隔离 | 一个模块出错不影响其他（try-catch 只包帧处理、不包整个循环） |
| 自愈 | 自动重连摄像头、自动重启 DMA、自动清告警 |
| 优雅降级 | UART 打不开？继续跑推理，只是不发结果（打印警告） |
| 可观测性 | 错误计数器、性能日志、CPU 温度——出问题时有线索 |
| 最后防线 | IWDG 硬件看门狗——软件全部失效时硬件兜底 |

## 要学到什么程度

- 理解每种故障模式的原因和应对策略
- 理解"宁可丢一帧，不让错误扩散"的设计哲学
- 知道硬件看门狗的意义：软件无法保证 100% 不出 bug
