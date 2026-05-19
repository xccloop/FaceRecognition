# STM32 端 — 待完成事项

## 当前状态

- FreeRTOS 运行正常，静态内存分配
- USART2 (PA2-TX, PA3-RX) 已初始化，115200-8-N-1
- ISR 接收字节放入 `xUartRxQueue`
- UART 接收任务有基础状态机，解析 8 字节测试帧（帧头 0xAA 0x55 + uint32 count + uint16 校验和）
- 校验通过后亮 PA0 LED
- 开发环境：Keil MDK-ARM，芯片 STM32F103C8T6

## 1. 替换为完整帧协议

**文件：** `User/main.c` 中的 `vUartRxTask`

当前是 8 字节定长测试帧，需要替换为变长帧协议：

```
帧格式: 0xAA | CMD | DIR | LEN_H | LEN_L | DATA[LEN] | XOR | 0x55
方向: 0x01=Pi→STM32, 0x02=STM32→Pi
校验: CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
```

**状态机改为两阶段：**

阶段 1 — 搜帧头：
```
循环读字节，直到遇到 0xAA，进入阶段 2
```

阶段 2 — 读帧：
```
state 0: 等待 CMD
state 1: 等待 DIR
state 2: 等待 LEN_H
state 3: 等待 LEN_L，计算总数据长度
state 4: 读取 LEN 字节的 DATA，遇到 0xBB 执行转义
         (0xBB 0x55 → 0xAA,  0xBB 0xAA → 0x55,  0xBB 0x44 → 0xBB)
state 5: 读取 XOR 校验字节，验证
state 6: 等待 0x55 帧尾，匹配则帧完成
         任一状态出错 → 回到阶段 1 重新搜帧头
```

**转义处理函数：**
```c
// 反转义：输入转义后的字节流，输出原始字节
// 返回实际写入的原始字节数
uint8_t unescape_byte(uint8_t b, uint8_t *out, uint8_t *escape_state);
```

## 2. 命令分发与处理

帧解析成功后，根据 CMD 调用对应 handler：

```c
typedef struct {
    uint8_t  cmd;
    uint8_t  dir;
    uint16_t len;
    uint8_t  data[64];  // 最大 64 字节
} Frame_t;

void handle_frame(const Frame_t *f) {
    switch (f->cmd) {
        case 0x10: handle_identify(f);   break;  // 识别成功
        case 0x11: handle_unknown(f);    break;  // 有人脸未识别
        case 0x12: handle_noface(f);     break;  // 无人脸
        case 0x13: handle_multiface(f);  break;  // 多人脸
        case 0x1F: handle_heartbeat(f);  break;  // 心跳
        default: break;
    }
}
```

### 各 handler 行为建议：

| 命令 | 数据 | LED 行为（PA0） | 其他 |
|------|------|-----------------|------|
| 0x10 IDENTIFY | "001,张三,0.92" | 绿灯常亮 2 秒 | 预留继电器触发 |
| 0x11 UNKNOWN | 空 | 黄灯闪烁 3 次 | 陌生人告警 |
| 0x12 NOFACE | 空 | LED 熄灭 | 待机状态 |
| 0x13 MULTIFACE | "3" | 红灯快闪 | 多人进入告警 |
| 0x1F HEARTBEAT | 空 | 短闪一次 | 更新心跳时间戳 |

## 3. 心跳超时检测

新增一个 FreeRTOS 定时器或独立任务：

- 记录最后一次收到有效帧（任意帧）的时间戳
- 每 1 秒检查一次，超时阈值 15 秒（3 倍心跳间隔）
- 超时则判定 Pi 离线：PA0 LED 以 1Hz 慢闪，所有输出复位到安全状态
- 收到新帧后恢复正常

```c
static TickType_t last_frame_ticks = 0;
#define HEARTBEAT_TIMEOUT_MS  15000

// 在主循环或定时器中检查
if ((xTaskGetTickCount() - last_frame_ticks) * portTICK_PERIOD_MS > HEARTBEAT_TIMEOUT_MS) {
    // 超时处理
}
```

## 4. UART 发送函数

当前没有发送功能。需要在 `uart1.hpp`（或新建 `uart.h`）中实现：

```c
// 发送原始字节
void uart1_send(const uint8_t *data, uint16_t len);

// 发送一帧（自动加帧头尾、转义、计算校验）
void uart1_send_frame(uint8_t cmd, uint8_t dir, const uint8_t *data, uint16_t len);
```

发送时的字节转义（编码侧）：
- 原始 0xAA → 0xBB 0x55
- 原始 0x55 → 0xBB 0xAA
- 原始 0xBB → 0xBB 0x44

**STM32 → Pi 可回复的命令：**
| 命令字 | 含义 | 触发时机 |
|--------|------|----------|
| 0x20 | 确认/开门 | 识别成功后回 ACK |
| 0x21 | 注册用户 | 按键触发或 Windows 后台转发 |
| 0x22 | 删除用户 | 按键触发 |
| 0x23 | 查询状态 | 按键触发 |

## 5. 动作执行（GPIO 扩展）

当前只用了 PA0 做 LED 指示。根据实际需求扩展：

```
PA0 — LED 状态指示（已用）
PA1 — 继电器控制（开门）
PA4 — 蜂鸣器（告警音）
PA5 — 按键 1（手动开门/注册）
PA6 — 按键 2（删除用户/复位）
```

**继电器控制示例：**
```c
void relay_on(void)  { GPIO_SetBits(GPIOA, GPIO_Pin_1); }
void relay_off(void) { GPIO_ResetBits(GPIOA, GPIO_Pin_1); }

void open_door(uint32_t duration_ms) {
    relay_on();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    relay_off();
}
```

## 6. 看门狗（IWDG）

防止程序跑飞。在 main() 初始化阶段使能独立看门狗：

```c
// IWDG: LSI 40KHz, 预分频 64 → 625Hz, 重载值 1250 → 2 秒超时
IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
IWDG_SetPrescaler(IWDG_Prescaler_64);
IWDG_SetReload(1250);
IWDG_Enable();
```

在 FreeRTOS 空闲钩子或一个低优先级任务中定期喂狗：
```c
IWDG_ReloadCounter();  // 喂狗
```

## 7. 文件结构

完成后的 Stm32/ 目录应包含：

```
Stm32/
├── Project.uvprojx          # Keil 工程文件
├── User/
│   ├── main.c               # 主程序（FreeRTOS 任务创建 + 调度）
│   ├── stm32f10x_it.c       # 中断服务（USART2 RXNE → 队列）
│   ├── stm32f10x_it.h
│   └── stm32f10x_conf.h
├── Core/                    # CMSIS
├── FreeRTOS/                # FreeRTOS 源码
├── Fwlib/                   # STM32 标准外设库
├── System/                  # 系统文件
├── Output/                  # 编译产物
├── doc/
│   └── tasks.md             # 本文档
└── Transmit.txt             # 当前测试发送端说明（参考）
```

## 8. 调试建议

1. 先用 PC 串口助手（如 SSCOM）模拟 Pi 发送帧，验证 STM32 解析正确
2. 帧协议调通后，再连树莓派联调
3. 在 `handle_frame()` 中加 printf（重定向到 USART1），方便观察收到的命令
