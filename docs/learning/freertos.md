# FreeRTOS

## 在这个项目中的作用

STM32 固件基于 FreeRTOS 实时操作系统，管理 UART 接收任务、心跳检测定时器、帧超时定时器。使用 RTOS 让多个逻辑并行执行，避免裸机程序的阻塞问题。

对应代码：`Stm32/User/main.c`、`Stm32/Protocol/cmd_handler.c`

## 项目中用到的具体知识

### 任务（Task）

项目中只有一个用户任务——UART 接收任务：

```c
// main.c
xTaskCreate(vUartRxTask,               // 任务函数
            "UartRx",                  // 任务名（调试用）
            configMINIMAL_STACK_SIZE * 2,  // 栈大小（加倍，给帧解析留空间）
            NULL,                      // 参数
            3,                         // 优先级（数字越大越优先）
            &xUartRxTaskHandle);       // 返回的任务句柄
```

任务函数是一个死循环，不断轮询 DMA ringbuffer → 逐字节喂帧解析器 → 完整帧交给 cmd_handler：

```c
static void vUartRxTask(void *pvParameters) {
    frame_parser_reset();
    for (;;) {
        avail = uart_dma_rx_available(hUart);
        if (avail == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));  // 阻塞等信号，最多 500ms
        }
        while (uart_dma_rx_available(hUart)) {
            // 逐字节解析...
        }
        IWDG_ReloadCounter();  // 喂狗：此任务跑完一轮 = 系统正常
    }
}
```

### 软件定时器（Software Timer）

两种周期性检查不需要独立任务，用 FreeRTOS 定时器更轻量：

```c
// 1s 心跳检查定时器
xHeartbeatTimer = xTimerCreate("HBCheck", pdMS_TO_TICKS(1000),  // 1 秒周期
                                pdTRUE,   // 自动重载
                                NULL, vHeartbeatTimerCallback);
xTimerStart(xHeartbeatTimer, 0);

// 5ms 帧超时定时器
xFrameTimeoutTimer = xTimerCreate("FrameTO", pdMS_TO_TICKS(5),
                                   pdTRUE, NULL, vFrameTimeoutTimerCallback);
xTimerStart(xFrameTimeoutTimer, 0);
```

- `pdTRUE`（自动重载）：每次触发后自动重新计时
- 定时器回调函数在 FreeRTOS 的 Timer 服务任务中执行，不能做耗时操作

### 任务通知（Task Notification）—— 轻量级同步

DMA IDLE 中断发生时，需要唤醒 UART 接收任务来处理数据。用任务通知比信号量更快、更省 RAM：

```c
// uart_dma.c — ISR 中
BaseType_t woken = pdFALSE;
if (h->rx_task) vTaskNotifyGiveFromISR(h->rx_task, &woken);
portYIELD_FROM_ISR(woken);

// main.c — 任务中等待
ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
```

`ulTaskNotifyTake(pdTRUE, ticks)` 的行为：
- `pdTRUE`：收到通知后把通知值清零
- 返回前如果超时没收到通知，返回 0；收到通知返回非 0
- 这里不关心返回值，因为回来后会再检查 `uart_dma_rx_available()`

### 静态内存分配

项目使用全静态分配，避免堆碎片化。需要自己提供 idle 任务和 timer 任务的栈空间：

```c
// main.c
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer, ...) {
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
}
```

### IWDG 喂狗

```c
IWDG_ReloadCounter();  // 在 vUartRxTask 的循环末尾调用
```

如果 vUartRxTask 卡死了（死锁、死循环），喂狗停止→IWDG 超时→系统复位。这利用了 FreeRTOS 的任务调度机制：只要任务还在跑循环，就说明核心路径正常。

## 要学到什么程度

- 理解任务的 5 个状态：running、ready、blocked、suspended、deleted
- 理解任务通知（轻量）和信号量/队列（通用）的区别与适用场景
- 理解软件定时器 vs 硬件定时器的区别：软件定时器受任务调度影响，精度低但省硬件资源
- 理解静态分配 vs 动态分配：嵌入式系统优先静态分配——编译时确定内存用量，无运行时碎片
