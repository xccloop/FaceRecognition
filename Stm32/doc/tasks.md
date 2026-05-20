# STM32 端 — 待完成事项

## 当前状态

- FreeRTOS 运行正常，静态内存分配
- USART2 (PA2-TX, PA3-RX) 已初始化，115200-8-N-1
- ISR 接收字节放入 `xUartRxQueue`
- **完整变长帧协议已实现**（0xAA/CMD/DIR/LEN_H/LEN_L/DATA/XOR/0x55，含转义/反转义）
- **命令分发已实现**（0x10 识别成功 → LED 常亮 + ACK 回复 / 0x11 陌生人 → 快闪 3 次 / 0x12 无人脸 → 熄灯 / 0x13 多人脸 → 快速闪烁 / 0x1F 心跳 → 短闪 + 更新时戳）
- **UART 发送已实现**（`uart_send()` 轮询 + `uart_send_frame()` 编码发送）
- **心跳超时检测已实现**（FreeRTOS 软件定时器，15 秒超时后 LED 1Hz 慢闪告警）
- **IWDG 看门狗已使能**（2 秒超时，500ms 喂狗定时器）
- PA0 LED 多模式指示（常亮/快闪/慢闪/熄灭）
- 开发环境：Keil MDK-ARM，芯片 STM32F103C8T6

## 1. 帧协议（已完成 ✓）

帧格式已实现为完整变长帧协议：

```
帧格式: 0xAA | CMD | DIR | LEN_H | LEN_L | DATA[LEN] | XOR | 0x55
方向: 0x01=Pi→STM32, 0x02=STM32→Pi
校验: CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
```

**两阶段状态机：**

阶段 1 — 搜帧头：循环读字节，直到遇到 0xAA，进入阶段 2

阶段 2 — 读帧：
- STATE_CMD → STATE_DIR → STATE_LEN_H → STATE_LEN_L
- STATE_DATA：逐字节读取，遇到 0xBB 执行转义解码
  - 0xBB 0x55 → 0xAA
  - 0xBB 0xAA → 0x55
  - 0xBB 0x44 → 0xBB
- STATE_XOR：校验 XOR，失败则丢弃整帧
- STATE_TAIL：验证 0x55 帧尾

任一状态出错 → 回到 STATE_SYNC 重新搜帧头

**转义编码（发送侧）：**
- 原始 0xAA → 0xBB 0x55
- 原始 0x55 → 0xBB 0xAA
- 原始 0xBB → 0xBB 0x44

## 2. 命令分发与处理（已完成 ✓）

Pi → STM32 命令：

| 命令字 | 含义 | LED 行为（PA0） | 回复 |
|--------|------|-----------------|------|
| 0x10 IDENTIFY | 识别成功 | 常亮 | 0x20 ACK |
| 0x11 UNKNOWN | 有人脸未识别 | 快闪 3 次(150ms) | 无 |
| 0x12 NOFACE | 无人脸 | 熄灭 | 无 |
| 0x13 MULTIFACE | 多人脸 | 快速闪烁 6 次(80ms) | 无 |
| 0x1F HEARTBEAT | 心跳 | 短闪 30ms + 更新时间戳 | 无 |

## 3. 心跳超时检测（已完成 ✓）

- 使用 FreeRTOS 软件定时器，每 1 秒检查一次
- 超时阈值：15 秒（3 倍心跳间隔）
- 任何有效帧（无论命令字）都更新时间戳 `xLastFrameTicks`
- 超时后 PA0 LED 以 1Hz（500ms 亮/灭）慢闪告警
- 收到新帧后自动恢复

## 4. UART 发送函数（已完成 ✓）

```c
void uart_send_byte(uint8_t b);       // 轮询发送单字节
void uart_send(const uint8_t *data, uint16_t len);  // 发送原始字节数组
void uart_send_frame(uint8_t cmd, uint8_t dir,       // 编码 + 发送完整帧
                     const uint8_t *data, uint16_t len);
```

STM32 → Pi 可回复的命令：

| 命令字 | 含义 | 触发时机 |
|--------|------|----------|
| 0x20 | 确认 | 收到 0x10 识别成功后自动回复 |

预留命令（待按键触发）：
| 0x21 | 注册用户 | 按键触发 |
| 0x22 | 删除用户 | 按键触发 |
| 0x23 | 查询状态 | 按键触发 |

## 5. 看门狗 IWDG（已完成 ✓）

```
IWDG: LSI 40kHz, 预分频 64 → 625Hz, 重载值 1250 → 2 秒超时
FreeRTOS 软件定时器每 500ms 喂狗一次
```

## 6. GPIO 扩展（待完成）

当前只用 PA0 做单 LED 多模式指示。后续可根据需求扩展：

```
PA0 — LED 状态指示（已用，多模式）
PA1 — 继电器控制（开门）
PA4 — 蜂鸣器（告警音）
PA5 — 按键 1（手动开门/注册）
PA6 — 按键 2（删除用户/复位）
```

## 7. 待完成

- [ ] 物理按键 + GPIO 扩展（PA1 继电器、PA4 蜂鸣器、按键）
- [ ] USART1 printf 重定向（调试用）
- [ ] 与树莓派 UART 联调（需树莓派到位）
- [ ] 长时间稳定性测试

## 8. 文件结构

```
Stm32/
├── Project.uvprojx           # Keil 工程文件
├── User/
│   ├── main.c                # 主程序（FreeRTOS + 帧协议 + 命令分发）
│   ├── stm32f10x_it.c        # 中断服务（USART2 RXNE → 队列）
│   ├── stm32f10x_it.h
│   └── stm32f10x_conf.h
├── FreeRTOS/
│   ├── include/
│   │   └── FreeRTOSConfig.h  # FreeRTOS 配置
│   ├── *.c                   # FreeRTOS 内核源码
│   └── portable/
├── Fwlib/                    # STM32 标准外设库
├── System/                   # 系统文件
├── Core/                     # CMSIS
├── Output/                   # 编译产物
├── doc/
│   ├── tasks.md              # 本文档
│   ├── workflow.md           # 开发工作流
│   ├── freertos-guide.md     # FreeRTOS 使用指南
│   └── pitfalls.md           # 踩坑记录
└── Transmit.txt              # 参考：旧测试发送端代码
```

## 9. 调试方法

1. 用 PC 串口助手（SSCOM）连接 STM32 的 USART2
2. 手动发送帧测试各命令：
   - `AA 10 01 00 05 30 30 31 2C E5 BC A0 E4 B8 89 2C 30 2E 39 32 XX 55` → 0x10 识别成功（数据="001,张三,0.92"的 UTF-8 + 计算XOR）
   - `AA 11 01 00 00 XX 55` → 0x11 陌生人
   - `AA 12 01 00 00 XX 55` → 0x12 无人脸
   - `AA 1F 01 00 00 XX 55` → 0x1F 心跳
3. 停止发心跳，15 秒后观察 LED 1Hz 慢闪 → 心跳超时告警
4. 恢复发任意帧 → LED 恢复正常
