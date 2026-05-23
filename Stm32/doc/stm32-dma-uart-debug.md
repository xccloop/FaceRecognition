# STM32 DMA+RingBuffer UART 集成调试历程

> 日期：2026-05-23
> 分支：`feat/stm32-dma-uart`
> 作者：向治昌

本文档记录将 FaceRecognition STM32 固件的轮询式 UART 接收替换为 DMA+RingBuffer 方案时遇到的问题及解决过程。

---

## 背景

原有固件使用 **USART2 RXNE 中断 + FreeRTOS 队列** 逐字节接收。虽然能工作，但 CPU 频繁响应中断，高波特率下存在丢字节风险。参照 `D:\DMA-RingBuffer UART Framework` 中的设计，决定引入：

- **DMA 循环模式** 自动接收字节到环形缓冲区
- **USART IDLE 中断** 检测帧间隔，通知任务取数据
- **RingBuffer** 作为 DMA 缓冲区与任务解析之间的数据桥梁
- **FreeRTOS 任务通知** 替代原有的队列逐字节传递

---

## 调试过程

### 第 1 轮：移植框架，RX 任务未执行

**现象：** 编译通过、烧录成功。串口打印启动横幅（"[1/4] GPIO OK" ~ "[4/4] Starting scheduler..."），15 秒后出现 `[ERROR] HEARTBEAT TIMEOUT`。但 `[RX] START` 从未出现。

**猜测 1：`xTaskCreate` 失败？**
- 栈不够？堆内存耗尽？任务创建失败后静默跳过（`configASSERT` 未定义）。

**猜测 2：调度器未启动？**
- `vTaskStartScheduler()` 之前的初始化出错导致 HardFault？

**猜测 3：任务创建成功但被 HardFault 立即杀死？**
- 访问非法地址（如未初始化的 DMA 寄存器）？

**诊断手段不足：** 问题在于我们完全不知道程序死在哪一步。没有 `configASSERT`，`HardFault_Handler` 只是死循环无输出。

### 第 2 轮：添加三层精确定位诊断（T0/T1/T2）

**修改：** 引入 `uart_direct_tx()` 函数 — 直接写 `USART1->DR` 寄存器，完全绕过 DMA/RingBuffer/FreeRTOS，确保在任何阶段（甚至调度器启动前）都能输出诊断信息。

| 诊断点 | 位置 | 含义 |
|---|---|---|
| **T0** | `vUartRxTask` 函数**第一条语句** | 任务入口是否到达 |
| **T1** | `xTaskCreate` 之后 | 任务创建成功/失败 |
| **T2** | `vTaskStartScheduler()` 之前 | Banner 后 TX 硬件是否仍可用 |

**验证结果：**

```
[T1] xTaskCreate OK (handle=valid)     ← 创建成功！
[MAIN] vTaskStartScheduler() now=[=RX]= ST=A=T   ← T2 + T0 一起出现但有乱码
========================================
  FaceRecognition STM32 v1.0
  DMA+RingBuffer UART / FreeRTOS
...
[DIAG] alive idle=0 cndtr=1024         ← 5 秒后出现！
```

**发现：**

1. ✅ T1：`xTaskCreate` 成功，任务已创建
2. ✅ T2：调度器启动前 TX 硬件正常
3. ✅ T0：`[RX] START` 出现了，但乱码为 `[=RX]= ST=A=T`
4. ✅ DIAG 每 5 秒输出一次，确认任务循环正常运行

**`[RX] START` 乱码原因：** 这是一个**一次性竞态**。`uart_direct_tx("[MAIN] vTaskStartScheduler() now\r\n")` 执行后立即调用 `vTaskStartScheduler()`，调度器启动后第一个运行的恰好是优先级最高的 RX 任务（优先级 3）。RX 任务首行 `uart_direct_tx("[RX] START\r\n")` 与主线程的最后一个字符几乎同时写 USART1->DR，导致字节交错。`uart_direct_tx` 无互斥保护，但这是启动瞬间的一次性问题，不影响后续运行。

**关键结论：DMA+RingBuffer+FreeRTOS 框架已跑通。任务调度、DMA 循环接收、RingBuffer 缓冲三层全部验证。**

### 第 3 轮：有框架但没收数据 — `idle=0`

**现象：** DIAG 显示 `idle=0 cndtr=1024`。

| 指标 | 含义 | 实际值 | 解读 |
|---|---|---|---|
| `idle` | USART IDLE 中断触发次数 | 0 | 没收到过完整帧 |
| `cndtr` | DMA 剩余字节计数 | 1024（=缓冲区满） | DMA 一个字节都没收到 |

**猜测 1：PC 没有发送数据。**
**猜测 2：USB-TTL 接线错误（TX/RX 接反、没共地）。**
**猜测 3：波特率不匹配。**

**验证：** 确认接线正确（USB-TTL TX → PA10 USART1_RX，GND 共地），串口助手设置 115200。

### 第 4 轮：收发成功！— 从看不出回复到找出真相

**现象：** 用串口助手发送 `AA 1F 01 00 00 1E 55`，DIAG 显示 `idle=1 cndtr=1016`，即 DMA 收到了 8 字节。同时出现 `[RX: 8 bytes]`！

但看不到 `[OK] HEARTBEAT received`。

**猜测：串口助手以 ASCII 文本模式发送，而非 HEX 模式。**

> `AA 1F 01 00 00 1E 55` 以 ASCII 发送时，实际发的是 22 个字符的 ASCII 码（`A=0x41, A=0x41, 空格=0x20, ...`），状态机在 STATE_SYNC 阶段寻找 `0xAA`，但收到的第一个字节是 `0x41`，永远匹配不到帧头。

**解决：切换到 HEX（十六进制）发送模式。**

### 第 5 轮：最终验证成功

**HEX 模式发送 `AA 1F 01 00 00 1E 55`：**

```
[19:05:17.876] AA 1F 01 00 00 1E 55 0A     ← PC 发送的心跳帧
[19:05:17.880] [RX: 8 bytes]               ← RingBuffer 收到 8 字节
[19:05:17.883] AA 20 02 00 02 48 42 2A 55  ← STM32 二进制 ACK 帧
               [OK] HEARTBEAT received      ← 纯文本调试输出
[19:05:22.880] [DIAG] alive idle=1 cndtr=1016  ← DIAG 一切正常
```

**二进制 ACK 帧解码验证：**

```
AA 20 02 00 02 48 42 2A 55
│  │  │  └┬┘ └┬┘  │  │
│  │  │   │   │   │  └─ 帧尾 0x55 ✓
│  │  │   │   │   └─ XOR = 0x20^0x02^0x00^0x02^0x48^0x42 = 0x2A ✓
│  │  │   │   └─ 数据 "HB" ✓
│  │  │   └─ LEN=2 ✓
│  │  └─ DIR_STM32_TO_PI=0x02 ✓
│  └─ CMD_ACK=0x20 ✓
└─ 帧头 0xAA ✓
```

---

## 最终状态

所有 5 条 Pi→STM32 命令均已验证：

| 命令 | HEX | 回复 | 状态 |
|---|---|---|---|
| 心跳 | `AA 1F 01 00 00 1E 55` | `[OK] HEARTBEAT received` + ACK | ✅ |
| 识别成功 | `AA 10 01 00 00 11 55` | `[OK] IDENTIFY: face recognized!` + ACK | ✅ |
| 未注册 | `AA 11 01 00 00 10 55` | `[WARN] UNKNOWN face detected!` | ✅ |
| 无人脸 | `AA 12 01 00 00 13 55` | `[INFO] NOFACE - standby` | ✅ |
| 多人脸 | `AA 13 01 00 00 12 55` | `[WARN] MULTIFACE: multiple faces detected!` | ✅ |

---

## 经验总结

### 关键教训

1. **`configASSERT` 必须实现。** 缺失时 `xTaskCreate` 失败静默跳过，没有任何输出，浪费大量排查时间。
2. **直接寄存器输出是无敌的诊断手段。** `uart_direct_tx` 绕过了所有可能出错的层级（DMA、RingBuffer、FreeRTOS 队列、调度器），在系统最脆弱的时候仍然可靠。
3. **CH340 在 DMA 模式下更稳定。** PL2303 直接 RXNE 中断方案下工作正常，但切换到 DMA 连续接收后 PL2303 可能因时序问题导致 DMA 偶尔卡在初始字节。
4. **串口工具的 HEX/ASCII 模式是经典的绊脚石。** 二进制协议必须用 HEX 模式发送，但串口助手默认通常是 ASCII。
5. **DMA CNDTR 是诊断利器。** `cndtr=1024` 表示 DMA 没收过数据（缓冲区中的剩余字节数 = 缓冲区大小），`cndtr=1016` 表示收了 8 字节，一目了然。

### 文件变更清单

| 文件 | 变更类型 | 说明 |
|---|---|---|
| `Stm32/User/main.c` | 重大修改 | 添加帧协议注释、三层诊断、DMA 驱动的 RX 任务 |
| `Stm32/User/ringbuffer.c/h` | **新增** | RingBuffer 实现 |
| `Stm32/User/uart_dma.c/h` | **新增** | DMA-UART 抽象层 |
| `Stm32/User/uart_port.c/h` | **新增** | SPL 硬件端口层（DMA + USART 初始化、中断处理） |
| `Stm32/User/uart_api.c/h` | **新增** | UART 便利层（互斥锁保护的阻塞/异步收发、printf） |
| `Stm32/User/stm32f10x_it.c` | 修改 | 添加 DMA 通道中断、USART IDLE 中断 |
| `Stm32/FreeRTOS/include/FreeRTOSConfig.h` | 修改 | 增大任务栈、添加 FreeRTOS API 头文件 |
| `Stm32/Project.uvprojx` | 修改 | 添加新源文件到 Keil 工程 |

---

> **文档维护：** 随 STM32 端调试过程持续更新。
