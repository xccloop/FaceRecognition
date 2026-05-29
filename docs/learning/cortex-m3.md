# ARM Cortex-M3 基础

## 在这个项目中的作用

STM32F103ZET6 使用 ARM Cortex-M3 内核。理解它的中断系统、NVIC、SysTick 是理解 FreeRTOS 和 DMA 驱动的前提。

对应代码：`Stm32/Core/core_cm3.c`、`Stm32/User/stm32f10x_it.c`

## 项目中用到的具体知识

### NVIC 中断优先级分组

```c
// main.c — main()
NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
```

Cortex-M3 的每个中断有"抢占优先级"和"子优先级"。优先级分组决定两者各占几位。`Group_4` 表示全部 4 位用于抢占优先级（0-15），无子优先级。

- **抢占优先级**：高优先级中断可以打断低优先级中断（嵌套）
- **子优先级**：同一抢占优先级的中断，子优先级高的先响应（但不打断）

项目中中断优先级配置（在 `uart_port.c` 中）：

| 中断 | 抢占优先级 | 作用 |
|------|-----------|------|
| DMA TX | 较低 | 发送完成中断，不紧急 |
| DMA RX | 较高 | 接收半满/全满中断，需及时处理 |
| USART IDLE | 高 | IDLE 中断——帧结束信号，最关键 |
| USART ERROR | 最高 | ORE/NE/FE 错误，需立即清理 |

### SysTick 定时器

FreeRTOS 用 SysTick（系统滴答定时器）产生心跳时钟。`configTICK_RATE_HZ` 通常设为 1000（1ms 一次中断），每次中断触发一次任务调度检查。

```c
// FreeRTOSConfig.h 中配置
#define configTICK_RATE_HZ  ((TickType_t)1000)
```

### 中断向量表

Cortex-M3 启动时从 Flash 0x08000000 处读取栈顶指针，然后跳到 Reset_Handler。文件中不显式操作，但理解这个机制有助于调试"程序不启动"的问题。

### 寄存器操作

STM32 的 SPL 库封装了寄存器操作，但关键寄存器仍需理解：

```c
// IWDG 配置直接操作寄存器
IWDG_SetPrescaler(IWDG_Prescaler_256);
IWDG_SetReload(625);
IWDG_ReloadCounter();
IWDG_Enable();
```

## 要学到什么程度

- 理解中断优先级的概念：抢占 vs 子优先级，为什么 IDLE 中断优先级要高于 TC/HT
- 理解 SysTick 的作用：FreeRTOS 的心跳来源
- 能看懂 STM32 参考手册中的寄存器描述
- 知道 Cortex-M3 的中断响应流程：硬件自动压栈（R0-R3,R12,LR,PC,xPSR）→ 查向量表 → 执行 ISR
