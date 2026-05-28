# STM32 — 门锁终端固件

## 技术栈

| 项目 | 说明 |
|------|------|
| MCU | STM32F103ZET6 (Cortex-M3, 64KB SRAM, 512KB Flash) |
| RTOS | FreeRTOS v10+, 静态内存分配 |
| 工具链 | Keil MDK (ARMCC v5), SPL 标准外设库 |
| 通信协议 | UART DMA + COBS/CRC8 二进制帧 |
| 硬件 | 3.5寸 TFTLCD (NT35310, FSMC 并口), W25Q128 SPI Flash, LED×2 |

## 功能

- UART DMA 接收 Pi 端识别结果
- TFTLCD 中文显示（GBK 全字库，SPI Flash）
- LED 状态指示（绿色成功/红色失败/闪烁超时）
- 独立看门狗 (IWDG, 4s 溢出)
- 串口心跳超时检测 (60s)

## 目录结构

```
Stm32/
├── User/main.c                   # FreeRTOS 入口
│   ├── UART RX 任务 (逐字节喂帧解析器)
│   ├── 1s 心跳检查定时器
│   ├── 5ms 帧超时定时器
│   └── IWDG 初始化
├── Comms/
│   ├── uart_dma.h/c              # DMA UART 驱动
│   │   ├── TX 链式 DMA
│   │   ├── RX 环形 DMA + IDLE 中断
│   │   └── SPSC 无锁 RingBuffer
│   ├── uart_port.h/c             # 硬件抽象层 (SPL→DMA)
│   └── ring_buffer.h/c           # RingBuffer 实现
├── Protocol/
│   ├── frame_protocol.h/c        # COBS+CRC8 帧编解码 + 解析器状态机
│   └── cmd_handler.h/c           # 命令分发
│       ├── IDENTIFY → 显示成功 + LED 绿
│       ├── UNKNOWN → 显示未注册 + LED 红
│       ├── NOFACE → 显示待机
│       ├── MULTIFACE → 显示多人 + LED 红
│       ├── HEARTBEAT → 回复 ACK
│       └── 60s 超时 → 显示超时 + LED 闪烁
├── BSP/
│   ├── LCD/                      # TFTLCD 驱动 + 汉字渲染
│   ├── LED/                      # LED 控制
│   ├── NORFLASH/                 # SPI NOR Flash (字库)
│   └── SPI/                      # SPI 驱动
├── Middlewares/TEXT/             # GB2312 全字库渲染
└── Project.uvprojx               # Keil 工程文件
```

## 通信协议

帧格式：`COBS(CMD + DIR + LEN_H + LEN_L + DATA) | CRC8 | 0x00`

| 命令 | 方向 | 说明 |
|------|------|------|
| 0x10 (IDENTIFY) | Pi→STM32 | 识别成功，DATA=GBK编码姓名 |
| 0x11 (UNKNOWN) | Pi→STM32 | 人脸未注册 |
| 0x12 (NOFACE) | Pi→STM32 | 无人脸 |
| 0x13 (MULTIFACE) | Pi→STM32 | 多人脸 |
| 0x1F (HEARTBEAT) | Pi→STM32 | 心跳 (每5s) |
| 0x20 (ACK) | STM32→Pi | 确认回复 |

## 编译烧录

Keil MDK 打开 `Stm32/Project.uvprojx` → 编译 → 烧录。

## 工程加固

| 机制 | 说明 |
|------|------|
| COBS 编码 | 0x00 天然帧分隔，无需超时判定帧尾 |
| CRC8 校验 | 多项式 0x07，帧级检错，损坏帧静默丢弃 |
| DMA ORE 恢复 | 溢出中断自动清标志→重启 DMA RX |
| IWDG 看门狗 | 4s 溢出，UART 任务每循环喂狗 |
| 静态内存 | FreeRTOS 全静态分配，无堆碎片化 |
