# DMA（直接内存访问）

## 在这个项目中的作用

STM32 的 UART 收发大量使用 DMA（Direct Memory Access），让数据传输不占用 CPU。以 115200 波特率持续接收数据时，如果每个字节都用 CPU 中断处理，CPU 会被频繁打断。DMA 解决了这个问题。

对应代码：`Stm32/Comms/uart_dma.c`

## 项目中用到的具体知识

### DMA 在 UART 收发中的角色

```
      TX DMA（链式）               RX DMA（环形）
┌─────────┐                  ┌─────────┐
│ TX Ring │──> DMA ──> UART  │ UART ──> DMA ──> RX Ring │
│ Buffer  │                  │         │       Buffer   │
└─────────┘                  └─────────┘
```

- **TX DMA**：CPU 往 ring buffer 写数据，DMA 自动把数据搬到 USART 发送寄存器
- **RX DMA**：DMA 自动把 USART 接收寄存器中的数据搬到 ring buffer，不用 CPU

### TX 链式 DMA（Chained DMA）

发送数据可能跨 ring buffer 的尾部：

```c
// uart_dma.c — tx_kick()
if (tail < head) {
    // 不跨边界：一次 DMA 传输完成
    uart_port_dma_tx_start(h->port, &h->tx_rb.buffer[ti], head - tail);
} else {
    // 跨边界：先发 [tail, capacity)，TC 中断后再发 [0, head)
    uint32_t seg1 = h->tx_rb.capacity - ti;
    h->chain_pending = (hi > 0);  // 标记"还有第二段"
    uart_port_dma_tx_start(h->port, &h->tx_rb.buffer[ti], seg1);
}
```

第一段发送完成后，TC（Transfer Complete）中断触发第二段发送：

```c
// on_tx_tc() ISR
if (h->chain_pending) {
    h->chain_pending = false;
    // 启动第二段 DMA
    uart_port_dma_tx_start(p, &h->tx_rb.buffer[0], s2);
}
```

### RX 环形 DMA（Circular Mode）

设置 DMA 为环形模式后，DMA 在 buffer 末尾自动回到开头继续写：

```c
// uart_port.c — uart_port_dma_rx_start()
DMA_InitStruct.DMA_Mode = DMA_Mode_Circular;  // 环形模式
```

通过 DMA 的 CNDTR 寄存器（当前剩余传输计数）计算新接收的字节数：

```c
// uart_dma.c — rx_update()
uint32_t curr = uart_port_rx_dma_remaining(h->port);  // 读 CNDTR
uint32_t delta;
if (curr <= prev) {
    delta = prev - curr;                               // 正常情况
} else {
    // CNDTR 变大 = DMA 发生了环形回绕
    delta = prev + (h->rx_rb.capacity - curr);
}
h->rx_prev_cndtr = curr;
rb_advance_head(&h->rx_rb, delta);  // 更新 ring buffer
```

### IDLE 中断 —— 帧结束信号

UART IDLE 中断在接收线空闲（超过一个字节时间没有新数据）时触发。这是帧结束的信号：

```c
// on_idle() ISR
rx_update(h);  // 最后一次计算剩余字节
BaseType_t woken = pdFALSE;
if (h->rx_task) vTaskNotifyGiveFromISR(h->rx_task, &woken);  // 唤醒 RX 任务
```

### ORE 错误恢复

ORE（Overrun Error）：CPU 没来得及读走数据，新数据又来了。发生时需要清标志、清 ring buffer、重启 DMA：

```c
// on_error() ISR
h->error_count++;      // 累计错误次数
h->error_restarts++;   // 累计重启次数
rb_flush(&h->rx_rb);   // 清空缓冲（错误期间的数据不可靠）
h->rx_prev_cndtr = h->rx_rb.capacity;
uart_port_dma_rx_stop(p);
uart_port_dma_rx_start(p, ...);  // 重新启动 DMA
```

## 要学到什么程度

- 理解 DMA 的核心价值：数据搬运不用 CPU，CPU 只在"传输完成"时处理
- 理解环形模式（Circular）与普通模式的差异
- 理解 CNDTR 寄存器的含义：传输剩余计数，通过它的变化推断新数据量
- 理解 IDLE 中断为什么是帧边界信号：一帧数据发完后，线上会短暂空闲
- 理解 ORE 错误的原因和处理流程
