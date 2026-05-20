# FreeRTOS 在 STM32 项目中的使用指南

> 本文面向不熟悉 FreeRTOS 的开发人员，说明它是什么、为什么要用、怎么配置、以及在本项目中具体怎么用的。

## 目录

1. [FreeRTOS 是什么](#1-freertos-是什么)
2. [为什么要用 FreeRTOS](#2-为什么要用-freeertos)
3. [核心概念](#3-核心概念)
4. [配置文件详解（FreeRTOSConfig.h）](#4-配置文件详解)
5. [本项目中的实际使用](#5-本项目中的实际使用)
6. [常用 API 速查](#6-常用-api-速查)

---

## 1. FreeRTOS 是什么

**FreeRTOS 是一个开源的实时操作系统（RTOS）内核**，专门为微控制器（MCU）设计。

打个比方：

```
裸机程序（Super Loop）           FreeRTOS
┌─────────────────────┐         ┌─────────────────────┐
│ while(1) {          │         │ Task1: 串口接收      │ ← 独立任务
│   do_A();           │         │ Task2: LED 闪烁      │ ← 独立任务
│   do_B();           │         │ Scheduler(调度器)    │
│   do_C();           │         │   ↓                 │
│ }                   │         │ 轮流分配 CPU 时间    │
│                     │         └─────────────────────┘
│ 问题：              │
│ - do_A 卡住，B/C   │         优势：
│   永远不会执行      │         - 任务隔离，互不影响
│ - 所有逻辑串行     │         - 高优先级任务可抢占
└─────────────────────┘         - 有成熟的同步机制
```

简单说：**FreeRTOS 让你的 MCU"看起来"能同时做多件事**。

## 2. 为什么要用 FreeRTOS

在本项目中，STM32 需要同时处理多件事：

1. **持续监听 UART**：树莓派随时可能发帧过来，不能丢字节
2. **LED 状态指示**：不同场景下 LED 要有不同的闪烁模式
3. **心跳超时检测**：定期检查是否长时间没收到帧
4. **看门狗喂狗**：每隔 500ms 喂一次狗
5. **命令处理**：收到帧后执行对应的动作逻辑

如果不用 RTOS 裸机写，你需要用超级循环 + 大量状态变量 + 定时器中断来模拟"同时"处理。代码会很快变成意大利面条。

用了 FreeRTOS：

| 需求 | FreeRTOS 机制 | 好处 |
|------|-------------|------|
| 串口接收 | **Queue（队列）** | ISR 只管丢字节，任务慢慢消费 |
| 定时检查 | **Software Timer（软件定时器）** | 不需要占用硬件定时器 |
| LED 闪烁 | 在空闲钩子中处理 | 不阻塞其他任务 |
| 喂狗 | 软件定时器 | 精确周期，不依赖任务循环 |

本质上就是：**把复杂的并发问题，拆成一个个独立的小逻辑单元**。

## 3. 核心概念

### 3.1 Task（任务）

任务就是一段独立的函数，有自己的栈空间。调度器按优先级轮流执行它们。

```c
void vMyTask(void *pvParameters) {
    for (;;) {  // 任务必须是个死循环
        // 做事情
        vTaskDelay(100);  // 主动让出 CPU 100ms
    }
}
```

### 3.2 Queue（队列）

队列是任务之间传递数据的管道。**线程安全的**，不需要手动加锁。

```c
// 创建：容量 64，每个元素是 uint8_t
QueueHandle_t q = xQueueCreate(64, sizeof(uint8_t));

// 发送（任务中）
xQueueSend(q, &data, portMAX_DELAY);

// 发送（ISR 中）— 必须用 FromISR 版本
xQueueSendFromISR(q, &data, &xHigherPriorityTaskWoken);

// 接收
xQueueReceive(q, &data, portMAX_DELAY);
```

在本项目中：**ISR 收到 UART 字节 → xQueueSendFromISR → 接收任务 xQueueReceive → 帧解析**

### 3.3 Software Timer（软件定时器）

不需要占用硬件定时器，由 FreeRTOS 在内部用一个硬件定时器（SysTick）模拟出任意多个软件定时器。

```c
// 创建：每 1000ms 触发一次，自动重载
TimerHandle_t t = xTimerCreate("name",
                                pdMS_TO_TICKS(1000),
                                pdTRUE,   // 自动重载
                                NULL,
                                vCallback);

// 启动
xTimerStart(t, 0);
```

在本项目中：
- `xHeartbeatTimer`：每秒检查心跳超时
- `xWatchdogTimer`：每 500ms 喂狗

### 3.4 中断与 FreeRTOS API

这是最容易踩坑的地方。关键规则：

```
┌──────────────────────────────────────────────────────┐
│  ISR 中只能调用以 "FromISR" 结尾的 FreeRTOS API     │
│  例如：xQueueSendFromISR()  ✓                        │
│        xQueueSend()         ✗  (会 HardFault)       │
└──────────────────────────────────────────────────────┘
```

且 ISR 的优先级必须 **>= configMAX_SYSCALL_INTERRUPT_PRIORITY**（数值上 >=，即逻辑优先级不高于该阈值）。

在本项目中：
- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 191`（数值，对应逻辑优先级 11）
- USART2 中断优先级 = 13（数值，对应逻辑优先级 13）
- 13 >= 11 ✓ → 可以在 ISR 中安全调用 FreeRTOS API

### 3.5 空闲钩子（Idle Hook）

当所有任务都阻塞时，调度器运行空闲任务。可以在空闲钩子中做低优先级的后台工作。

在本项目中：空闲钩子用于 LED 慢闪（心跳超时告警）。因为闪烁不需要精确计时，在空闲钩子中简单检查一下时间戳就行，不占用额外任务栈。

## 4. 配置文件详解

文件位置：`Stm32/FreeRTOS/include/FreeRTOSConfig.h`

### 4.1 调度器核心配置

```c
#define configUSE_PREEMPTION         1   // 抢占式调度（高优先级任务可以打断低优先级）
#define configCPU_CLOCK_HZ           ( SystemCoreClock )  // CPU 主频（72MHz）
#define configTICK_RATE_HZ           1000  // 系统节拍 1000Hz（1ms 一个 tick）
#define configMAX_PRIORITIES         5     // 最大优先级数（0~4，0 最低）
#define configMINIMAL_STACK_SIZE     128   // 最小任务栈（字为单位，128 字 = 512 字节）
```

关键参数说明：

| 参数 | 本项目的值 | 含义与影响 |
|------|----------|-----------|
| `configTICK_RATE_HZ` | 1000 | 1ms 分辨率。越大越精确但中断越频繁。1000 是嵌入式常用值 |
| `configMAX_PRIORITIES` | 5 | 支持 5 个优先级（0-4）。本项目任务少，够用 |
| `configMINIMAL_STACK_SIZE` | 128 | 每个任务至少 128 字的栈。过小 → 栈溢出（触发 Hook） |

### 4.2 内存管理

```c
#define configTOTAL_HEAP_SIZE        ( 7 * 1024 )  // 总堆大小 7KB
#define configSUPPORT_STATIC_ALLOCATION  1          // 支持静态分配
#define configSUPPORT_DYNAMIC_ALLOCATION 1          // 支持动态分配
```

**本项目同时使用静态和动态分配：**

- 空闲任务和定时器任务：**静态分配**（通过 `vApplicationGetIdleTaskMemory` 等钩子提供内存）
- 队列和定时器：**动态分配**（`xQueueCreate` / `xTimerCreate` 从堆中分配）

为什么混用？静态分配避免堆碎片，但不方便。简单的队列和定时器用动态分配更省心。

### 4.3 调试与保护

```c
#define configCHECK_FOR_STACK_OVERFLOW    2     // 栈溢出检测（等级 2，最严格）
#define configUSE_MALLOC_FAILED_HOOK      1     // malloc 失败时触发钩子
```

- **栈溢出检测等级 2**：任务创建时将栈填充为已知值（0xA5），切换任务时检查栈底是否被覆盖。在本项目中，栈溢出钩子会让程序死循环（便于调试器定位）。
- **malloc 失败钩子**：如果 `xQueueCreate` 等返回 NULL，会触发。本项目中没有显式检查 malloc 返回值——如果真失败了，钩子会捕获。

### 4.4 软件定时器

```c
#define configUSE_TIMERS             1
#define configTIMER_TASK_PRIORITY    4     // 定时器任务优先级（最高）
#define configTIMER_QUEUE_LENGTH     10    // 定时器命令队列长度
#define configTIMER_TASK_STACK_DEPTH ( configMINIMAL_STACK_SIZE * 2 )  // 256 字
```

定时器回调在**定时器任务**的上下文中执行，不是在中断上下文中。所以回调里可以调用普通 FreeRTOS API（不用 FromISR 版本）。

定时器任务优先级设为 4（最高），确保喂狗和心跳检测不会被其他任务阻塞。

### 4.5 中断管理

```c
#define configKERNEL_INTERRUPT_PRIORITY          255  // 内核中断优先级（最低）
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     191  // 可调 API 的 ISR 最低优先级

#define vPortSVCHandler      SVC_Handler
#define xPortPendSVHandler   PendSV_Handler
#define xPortSysTickHandler  SysTick_Handler
```

STM32 Cortex-M3 的优先级数值越大 = 逻辑优先级越低。

```
逻辑优先级高 ←────────────────────→ 逻辑优先级低
    0 .......................... 15
    │                              │
    │←─ 可调用 FreeRTOS API ──────→│← 不可调用 ─→│
    │   (优先级 >= 191, 即 <= 11)   │              │
```

本项目 USART2 中断优先级设为 13（192 对应 12，13=数值 208 > 191，在安全范围内）。

## 5. 本项目中的实际使用

### 5.1 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                     FreeRTOS 调度器                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ UART RX Task │  │ Timer Task   │  │  Idle Task   │  │
│  │  (优先级 3)   │  │ (优先级 4)    │  │  (优先级 0)   │  │
│  │              │  │              │  │              │  │
│  │ 帧协议状态机  │  │ 心跳超时检测  │  │ LED 慢闪     │  │
│  │ 命令分发     │  │ 看门狗喂狗   │  │              │  │
│  └──────┬───────┘  └──────────────┘  └──────────────┘  │
│         │                                               │
│    ┌────▼────┐                                          │
│    │  Queue   │ ◄── ISR 写入                             │
│    │ (64字节) │                                          │
│    └────┬────┘                                          │
│         │                                               │
│  ┌──────▼──────┐                                        │
│  │ USART2 ISR  │                                        │
│  │ (优先级 13)  │                                        │
│  └─────────────┘                                        │
└─────────────────────────────────────────────────────────┘
```

### 5.2 数据流

```
PC/树莓派
   │ UART 字节流
   ▼
USART2_IRQHandler()           ← 硬件中断，每收到一个字节触发
   │ xQueueSendFromISR()      ← 字节丢进队列
   ▼
xUartRxQueue (FreeRTOS Queue)
   │ xQueueReceive()          ← 任务阻塞等待
   ▼
vUartRxTask()                 ← UART 接收任务
   │ 帧协议状态机解析
   │ 转义解码
   ▼
dispatch_frame()              ← 命令分发
   ├── 0x10 → LED_ON + uart_send_frame(CMD_ACK)
   ├── 0x11 → LED 快闪 3 次
   ├── 0x12 → LED_OFF
   ├── 0x13 → LED 快速闪烁
   └── 0x1F → LED 短闪 + 更新时间戳
```

### 5.3 定时器任务

定时器任务由 FreeRTOS 内部自动创建（优先级 4，最高），负责执行所有软件定时器的回调：

```c
// 心跳检测定时器：1000ms 周期
vHeartbeatTimerCallback()
    → 检查 xLastFrameTicks 是否超过 15 秒
    → 超时则 led_pattern_active = 1

// 看门狗喂狗定时器：500ms 周期
vWatchdogTimerCallback()
    → IWDG_ReloadCounter()
```

### 5.4 静态内存分配

空闲任务和定时器任务使用静态分配，在 main.c 中提供内存：

```c
void vApplicationGetIdleTaskMemory(...) {
    static StaticTask_t xIdleTaskTCB;         // TCB（任务控制块）
    static StackType_t  uxIdleTaskStack[128]; // 栈空间
    // 返回给 FreeRTOS
}

void vApplicationGetTimerTaskMemory(...) {
    static StaticTask_t xTimerTaskTCB;
    static StackType_t  uxTimerTaskStack[256];
}
```

## 6. 常用 API 速查

### 任务管理

```c
TaskHandle_t h;
xTaskCreate(vTask, "name", stack_size, params, priority, &h);  // 动态创建
vTaskDelay(pdMS_TO_TICKS(100));            // 延时 100ms
vTaskDelayUntil(&last, pdMS_TO_TICKS(50)); // 精确周期延时
vTaskSuspend(h);                            // 挂起任务
vTaskResume(h);                             // 恢复任务
```

### 队列

```c
QueueHandle_t q = xQueueCreate(64, sizeof(MyStruct));  // 创建队列（64 个元素）
xQueueSend(q, &data, portMAX_DELAY);                   // 发送（阻塞直到有空位）
xQueueSend(q, &data, 0);                               // 发送（非阻塞，满了返回 errQUEUE_FULL）
xQueueReceive(q, &data, portMAX_DELAY);                // 接收（阻塞直到有数据）

// ISR 版本
BaseType_t woken;
xQueueSendFromISR(q, &data, &woken);
portYIELD_FROM_ISR(woken);   // 如果唤醒了更高优先级任务，触发上下文切换
```

### 软件定时器

```c
TimerHandle_t t = xTimerCreate("name", pdMS_TO_TICKS(500), pdTRUE, NULL, callback);
xTimerStart(t, 0);            // 启动（0 = 不等待）
xTimerStop(t, 0);             // 停止
xTimerReset(t, 0);            // 重置（重新计时）
xTimerChangePeriod(t, pdMS_TO_TICKS(1000), 0);  // 修改周期
```

### 临界区保护

```c
taskENTER_CRITICAL();
// 访问共享变量的代码（不会被中断打断）
taskEXIT_CRITICAL();
```

### 调试

```c
UBaseType_t uxTaskGetStackHighWaterMark(NULL);  // 获取当前任务的栈剩余最小值
UBaseType_t uxTaskGetNumberOfTasks(void);       // 当前任务数量
```

---

> **一句话总结：FreeRTOS 让你的 MCU 能同时做多件事，Queue 负责数据传输，Timer 负责定时任务，配置主要管优先级、内存和调试保护。**
