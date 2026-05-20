# STM32 项目开发工作流

> 面向维护者和新加入的开发人员，说明从拿到代码到烧录运行的完整流程。

## 目录

1. [项目概述](#1-项目概述)
2. [开发环境搭建](#2-开发环境搭建)
3. [项目结构](#3-项目结构)
4. [开发流程](#4-开发流程)
5. [编译与烧录](#5-编译与烧录)
6. [调试方法](#6-调试方法)
7. [测试流程](#7-测试流程)
8. [Git 工作流](#8-git-工作流)

---

## 1. 项目概述

STM32 端在人脸识别系统四端架构中担任**通信与执行**角色：

```
树莓派(UART) ──帧协议──► STM32 ──► LED/继电器/蜂鸣器
                           │
                           └──► 回复ACK/指令给树莓派
```

- **MCU:** STM32F103C8T6 (Cortex-M3, 64KB SRAM, 128KB Flash)
- **RTOS:** FreeRTOS（静态内存分配）
- **库:** STM32 FWlib 3.5（标准外设库）
- **通信:** USART2, 115200-8-N-1, 变长帧协议
- **开发环境:** Keil MDK-ARM (μVision)

## 2. 开发环境搭建

### 2.1 必需软件

| 软件 | 版本 | 用途 |
|------|------|------|
| Keil MDK-ARM | 5.x | IDE + 编译 + 调试 |
| STM32F1xx DFP | Keil Pack | 芯片支持包 |
| ST-LINK Utility | 最新 | 烧录工具（备选） |
| SSCOM / PuTTY | 任意 | 串口调试助手 |

### 2.2 硬件连接

```
STM32F103C8T6 (Blue Pill)
    PA2 (TX) ──► USB-TTL 模块 RX
    PA3 (RX) ◄── USB-TTL 模块 TX
    PA0       ──► LED (通过 220Ω 限流电阻到 GND)
    GND       ──► USB-TTL 模块 GND

烧录: ST-LINK V2
    SWDIO ──► PA13 (SWDIO)
    SWCLK ──► PA14 (SWCLK)
    GND   ──► GND
    3.3V  ──► 3.3V
```

### 2.3 打开工程

1. 启动 Keil μVision
2. Project → Open Project → 选择 `Stm32/Project.uvprojx`
3. 确认芯片型号为 STM32F103C8

## 3. 项目结构

```
Stm32/
├── Project.uvprojx          # Keil 工程文件（入口）
├── Project.uvoptx
│
├── User/                    # 用户代码
│   ├── main.c               # ★ 主程序：FreeRTOS + 帧协议 + 命令分发
│   ├── stm32f10x_it.c       # 中断服务函数（USART2 RXNE）
│   ├── stm32f10x_it.h
│   └── stm32f10x_conf.h     # 外设库头文件包含配置
│
├── FreeRTOS/                # FreeRTOS 内核（v10.x）
│   ├── include/             # 内核头文件
│   │   ├── FreeRTOS.h
│   │   ├── task.h
│   │   ├── queue.h
│   │   ├── timers.h
│   │   └── FreeRTOSConfig.h # ★ 项目 FreeRTOS 配置
│   ├── tasks.c              # 任务调度
│   ├── queue.c              # 队列
│   ├── timers.c             # 软件定时器
│   ├── event_groups.c       # 事件组
│   ├── list.c               # 链表
│   ├── heap_4.c             # 内存管理（heap_4）
│   └── portable/
│       └── RVDS/ARM_CM3/    # Cortex-M3 移植层
│           ├── port.c
│           └── portmacro.h
│
├── Fwlib/                   # STM32 标准外设库 3.5
│   ├── inc/                 # 头文件
│   └── src/                 # 源文件
│
├── System/                  # 系统启动文件
│   ├── system_stm32f10x.c
│   └── startup_stm32f10x_md.s
│
├── Core/                    # CMSIS Core
│   └── core_cm3.c
│
├── Output/                  # 编译产物（.o, .map, .hex）
├── doc/                     # 文档
│   ├── tasks.md             # 待完成事项
│   ├── workflow.md          # ★ 本文档
│   ├── freertos-guide.md    # FreeRTOS 使用指南
│   └── pitfalls.md          # 踩坑记录
│
└── Transmit.txt             # 旧测试发送端代码（参考）
```

## 4. 开发流程

### 4.1 修改代码

核心文件只有两个：

- **`User/main.c`** — 所有业务逻辑：外设初始化、帧协议、命令分发、看门狗
- **`User/stm32f10x_it.c`** — USART2 中断接收字节入队

### 4.2 main.c 结构说明

```c
main()
 ├── NVIC_PriorityGroupConfig(4)          // 优先级分组
 ├── PA0_LED_Init()                        // GPIO 初始化
 ├── xQueueCreate()                        // 创建 FreeRTOS 队列
 ├── USART2_Init(115200)                   // 串口初始化
 ├── IWDG_Init()                           // 看门狗初始化
 ├── xTaskCreate(vUartRxTask, ...)         // 创建 UART 接收任务
 ├── xTimerCreate + xTimerStart            // 心跳超时检测定时器
 ├── xTimerCreate + xTimerStart            // 看门狗喂狗定时器
 └── vTaskStartScheduler()                 // 启动调度器
```

### 4.3 关键函数一览

| 函数 | 文件 | 作用 |
|------|------|------|
| `vUartRxTask()` | main.c | UART 接收任务，帧协议状态机 |
| `dispatch_frame()` | main.c | 命令分发 |
| `uart_send_frame()` | main.c | 编码 + 发送完整帧 |
| `frame_encode()` | main.c | 帧编码（含转义） |
| `unescape_byte()` | main.c | 反转义解码 |
| `vHeartbeatTimerCallback()` | main.c | 心跳超时检测 |
| `vWatchdogTimerCallback()` | main.c | IWDG 喂狗 |
| `USART2_IRQHandler()` | stm32f10x_it.c | USART2 中断接收字节入队 |

## 5. 编译与烧录

### 5.1 编译

1. Keil 中按 **F7**（Build）编译
2. 确认 Output 窗口显示 `0 Error(s), 0 Warning(s)`
3. 编译产物在 `Output/` 目录：
   - `Project.hex` — 烧录文件
   - `Project.map` — 内存映射（调试用）

### 5.2 烧录

**方法 A：Keil 直接烧录（推荐）**
1. 连接 ST-LINK
2. Flash → Download（或按 F8）
3. 确认 `Flash Load finished`

**方法 B：ST-LINK Utility**
1. 打开 ST-LINK Utility
2. Target → Connect
3. File → Open File → 选择 `Output/Project.hex`
4. Target → Program & Verify

### 5.3 常见编译问题

| 问题 | 解决 |
|------|------|
| `Undefined symbol IWDG_xxx` | 确认 `stm32f10x_conf.h` 中 `#include "stm32f10x_iwdg.h"` 未被注释 |
| `Undefined symbol vTaskDelay` 等 | 确认 FreeRTOS 源文件已加入工程 |
| Flash 空间不足 | 检查 `configTOTAL_HEAP_SIZE` 和 `configMINIMAL_STACK_SIZE` |

## 6. 调试方法

### 6.1 串口调试（不需要树莓派）

用 USB-TTL 模块连接 STM32，PC 端打开串口助手（SSCOM），设置：

- 波特率：115200
- 数据位：8
- 停止位：1
- 校验：无
- HEX 发送模式

**发送测试帧：**

```
# 心跳帧: AA 1F 01 00 00 2E 55
# 无人脸帧: AA 12 01 00 00 13 55
# 识别成功帧（数据="001,张三,0.92"）:
# AA 10 01 00 12 30 30 31 2C E5 BC A0 E4 B8 89 2C 30 2E 39 32 XX 55
#   (XX = XOR，需计算)
```

观察 PA0 LED：
- 收到 0x10 → 常亮 + 回 0x20 ACK
- 收到 0x11 → 快闪 3 次
- 收到 0x12 → 熄灭
- 收到 0x1F → 短闪一次
- 15 秒无帧 → 1Hz 慢闪

### 6.2 硬件调试（ST-LINK）

1. Keil 中 Debug → Start/Stop Debug Session（Ctrl+F5）
2. 可设置断点在：
   - `dispatch_frame()` → 观察收到的命令
   - `USART2_IRQHandler()` → 观察中断触发
   - 定时器回调 → 确认喂狗和心跳检测正常

### 6.3 printf 重定向（可选）

如需 printf 调试输出到 USART1，需要：

```c
// 在 main.c 中添加：
int fputc(int ch, FILE *f) {
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, (uint8_t)ch);
    return ch;
}
```

并在 Keil 工程选项 Target 选项卡中勾选 "Use MicroLIB"。

## 7. 测试流程

### 7.1 单元测试（无树莓派）

| 测试项 | 操作 | 预期结果 |
|--------|------|---------|
| 帧头同步 | 发送 `55 AA 10 01 00 00 11 55`（前面有 0x55 垃圾字节） | 正确跳过 0x55，从 0xAA 开始解析 |
| 正常帧 | 发送合法帧 | 对应 handler 触发 |
| 校验失败 | 发送帧但 XOR 字节故意写错 | 帧被丢弃，LED 无反应 |
| 数据超长 | 发送 LEN > 64 的帧 | 帧被丢弃 |
| 转义测试 | DATA 区含 0xAA/0x55/0xBB | 正确反转义 |
| 心跳超时 | 发送一次心跳后 15 秒不发 | LED 1Hz 慢闪 |
| 超时恢复 | 心跳超时后发任意帧 | LED 恢复正常 |
| ACK 回复 | 发送 0x10 识别成功帧 | 串口接收到 0x20 ACK 帧 |

### 7.2 联调（需要树莓派）

1. 树莓派 GPIO14(TX)→PA3(RX), GPIO15(RX)→PA2(TX)，共地
2. 树莓派运行 `facerec` 实时识别程序
3. 观察 STM32 LED 随识别结果变化
4. 串口抓包验证帧协议正确

## 8. Git 工作流

### 8.1 分支策略

```
main ──── 稳定发布分支
  └── feature/xxx ── 功能开发分支
```

### 8.2 Commit 规范

```
feat(stm32): 实现完整变长帧协议与转义处理
fix(stm32): 修复 IWDG 时钟未使能导致 HardFault
docs(stm32): 添加开发工作流文档
```

### 8.3 提交前检查清单

- [ ] 编译 0 Error 0 Warning
- [ ] 烧录运行正常
- [ ] 代码风格一致（缩进、命名）
- [ ] 相关文档已更新（tasks.md）
