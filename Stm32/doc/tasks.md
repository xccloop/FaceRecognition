# STM32 固件 — 任务清单与进度

> 最后更新：2026-05-23（DMA UART 集成完成）

---

## 已完成

### 基础驱动层

- [x] FreeRTOS 静态内存分配（configSUPPORT_STATIC_ALLOCATION=1）
- [x] LED 驱动 — GPIOB.5、GPIOE.5（推挽输出，低电平点亮）
- [x] 独立看门狗 IWDG — LSI 40kHz, 64 分频, 1250 重载 ≈ 2s
- [x] 时钟配置 — HSE 8MHz, PLL 72MHz

### DMA + RingBuffer UART 框架（新模块）

- [x] `Stm32/User/ringbuffer.c/h` — 通用环形缓冲区（2 的幂大小，不丢字节）
- [x] `Stm32/User/uart_port.c/h` — SPL 硬件端口层（DMA1 C5 循环模式 + USART1 IDLE 中断）
- [x] `Stm32/User/uart_dma.c/h` — DMA-UART 抽象层（以 RingBuffer 为 backend）
- [x] `Stm32/User/uart_api.c/h` — UART 便利层（互斥锁保护的阻塞/异步收发 + printf 重定向）
- [x] `stm32f10x_it.c` — DMA1_CH5 ISR、USART1 IDLE ISR（设置 g_idle_count 计数器 + 清理 IDLE 标志 + 通知任务）

### 通信协议

- [x] 二进制变长帧格式：`0xAA CMD DIR LEN_H LEN_L DATA... XOR 0x55`
- [x] 0xBB 转义机制（0xAA→BB 55, 0x55→BB AA, 0xBB→BB 44）
- [x] XOR 校验（帧头帧尾不参与）
- [x] 有限状态机帧解析（7 状态：SYNC/CMD/DIR/LEN_H/LEN_L/DATA/XOR/TAIL）
- [x] 5 条命令处理器：IDENTIFY(0x10), UNKNOWN(0x11), NOFACE(0x12), MULTIFACE(0x13), HEARTBEAT(0x1F)
- [x] STM32→Pi ACK 回复（CMD_ACK 0x20 + 数据 "HB"）
- [x] 15s 心跳超时检测（文本告警 + 恢复自动清除）

### 测试验证

- [x] PC 串口助手 HEX 模式联调 — 全 5 条命令通过
- [x] DMA CNDTR 诊断：idle>0 确认收包, cndtr 变化确认字节数
- [x] 直接寄存器 TX 诊断：预调度器/任务入口三层定位（T0/T1/T2）
- [x] 任务存活诊断：5s 超时轮询打印 `[DIAG] alive idle=X cndtr=YYYY`

---

## 待完成

- [ ] 继电器 GPIO 控制及开门时序（需配合 Pi 端命令）
- [ ] 树莓派 UART 硬件连接与全链路联调
- [ ] 蜂鸣器/按键/OLED 等外设扩展
- [ ] EEPROM 或 Flash 存储配置参数

---

## 文件结构（变更后）

```
Stm32/
├── Project.uvprojx                # Keil MDK 工程
├── Project.uvoptx                 # Keil 工程配置
├── Core/
│   └── startup_stm32f103xe.s      # 启动文件
├── Fwlib/                         # STM32 标准外设库 (SPL 3.5)
│   └── inc/
│   └── src/
├── FreeRTOS/
│   ├── src/                       # FreeRTOS 内核源码
│   └── include/
│       └── FreeRTOSConfig.h       # ★ 静态分配使能, 栈/堆配置
├── User/
│   ├── main.c                     # ★ 主程序（协议注释 + 任务 + 帧解析 + 命令分发）
│   ├── ringbuffer.c               # ★ 新增 — RingBuffer 实现
│   ├── ringbuffer.h
│   ├── uart_port.c                # ★ 新增 — SPL 硬件端口层
│   ├── uart_port.h
│   ├── uart_dma.c                 # ★ 新增 — DMA-UART 抽象层
│   ├── uart_dma.h
│   ├── uart_api.c                 # ★ 新增 — UART 便利层
│   ├── uart_api.h
│   ├── stm32f10x_it.c             # ★ 修改 — DMA + IDLE ISR
│   └── stm32f10x_conf.h
├── Output/                        # 编译输出 (.axf/.hex/.bin)
└── doc/
    ├── stm32-dma-uart-debug.md    # ★ 新增 — DMA 集成调试历程
    ├── tasks.md                   # 本文件
    ├── pitfalls.md                # 避坑指南
    ├── Transmit.txt               # 帧协议原始定义
    └── ...
```
