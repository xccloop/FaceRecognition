# 二进制帧协议设计

## 在这个项目中的作用

从零设计了一套串口二进制帧协议，用于树莓派 ↔ STM32 之间的数据通信。协议层包含：COBS 编码 + CRC8 校验 + 0x00 帧分隔符。

对应代码：
- 协议定义：`Stm32/Protocol/frame_protocol.h`
- STM32 实现：`Stm32/Protocol/frame_protocol.c`
- Pi 实现：`Linux/Model/src/uart_protocol.cpp`
- Python 参考实现 + 测试：`tests/protocol/protocol_ref.py`、`tests/protocol/test_frame.py`

## 帧格式

```
┌───────────────────────────┬────────┬───────┐
│ COBS(payload)             │ CRC8   │ 0x00  │
└───────────────────────────┴────────┴───────┘
  变长（≤ MAX_COBS_LEN）      1 byte   1 byte
```

其中 payload：

```
┌─────┬─────┬────────┬────────┬──────────────┐
│ CMD │ DIR │ LEN_H  │ LEN_L  │ DATA(0~64B)  │
│ 1B  │ 1B  │ 1B     │ 1B     │ 变长          │
└─────┴─────┴────────┴────────┴──────────────┘
```

### 字段含义

| 字段 | 大小 | 含义 |
|------|------|------|
| CMD | 1 byte | 命令字：0x10=IDENTIFY, 0x11=UNKNOWN, 0x12=NOFACE, 0x13=MULTIFACE, 0x1F=HEARTBEAT, 0x20=ACK |
| DIR | 1 byte | 方向：0x01=Pi→STM32, 0x02=STM32→Pi |
| LEN | 2 bytes | DATA 的字节数（大端序，0~64） |
| DATA | 0~64 bytes | 变长载荷（如识别成功时的人名 GBK 编码） |

### 协议常量

```c
#define MAX_DATA_LEN     64        // 数据最大 64 字节
#define FRAME_DELIMITER  0x00      // 帧分隔符
#define FRAME_BYTE_TIMEOUT_MS  5   // 字节间超时 5ms
```

## 设计与实现要点

### 1. 帧分隔：为什么用 0x00

COBS 编码保证编码后的数据内部不含 0x00，因此 0x00 可以用作唯一、无歧义的帧结束标记。接收方看到 0x00 就知道一帧结束。

### 2. 分层设计

```
应用层: CMD + DATA（命令+数据）
  ↓
传输层: COBS | CRC8 | 0x00（帧编码/解码）
  ↓
链路层: UART DMA（字节传输）
```

每一层只关心自己的职责：链路层负责字节收发、传输层负责帧定界和校验、应用层负责命令语义。

### 3. 逐字节解析状态机

接收端不知道帧什么时候到，不能指望一次收齐。实现为逐字节状态机：

```c
// frame_protocol.c — frame_parser_feed()
// 状态 1: STATE_WAIT_DELIMITER — 跳过 0x00，收到非 0x00 → 进入 DATA
// 状态 2: STATE_DATA           — 收集字节，收到 0x00 → 进入 GOT_DELIMITER
// 状态 3: STATE_GOT_DELIMITER  — 解码 + CRC 校验 + 返回结果 → 回到 WAIT

switch (g_state) {
case STATE_WAIT_DELIMITER:
    if (ch == 0x00) break;      // 跳过前导分隔符
    g_buf[g_buf_count++] = ch;
    g_state = STATE_DATA;
    break;

case STATE_DATA:
    if (ch == 0x00) {
        // 帧结束，跳到处理逻辑
        goto process_frame;
    }
    g_buf[g_buf_count++] = ch;  // 继续收集
    break;
}
```

### 4. 帧解码校验链

收到完整帧后，依次检查：

1. **最小长度检查**：至少 CRC(1) + COBS 最小编码(1) = 2 字节
2. **COBS 解码**：解码失败 → 丢弃
3. **Payload 长度检查**：4 ≤ 长度 ≤ MAX_DATA_LEN + 4
4. **CRC8 校验**：收到的 CRC vs 计算的 CRC → 不匹配 → 丢弃
5. **LEN 字段一致性**：LEN 字段值 == 实际 DATA 长度 → 不一致 → 丢弃

任一步失败都丢弃整帧，并置 `error_flag`。

### 5. 字节间超时保护

如果发送端崩溃、信号中断或传输被干扰，接收端可能收到半个帧。5ms 字节间超时保证状态机不会永远卡在 "STATE_DATA"：

```c
// 由 5ms FreeRTOS 定时器周期性调用
void frame_parser_check_timeout(uint32_t current_tick) {
    if ((current_tick - g_last_tick) >= 5) {
        g_state = STATE_WAIT_DELIMITER;  // 丢弃半帧
        g_buf_count = 0;
    }
}
```

115200 波特率下，一个字节约 87μs，两个字节间隔 5ms 意味着至少丢失了约 57 个字节——基本可以确定帧传输中止了。

### 6. 命令定义（应用层语义）

```c
// frame_protocol.h
#define CMD_IDENTIFY   0x10   // 识别成功，DATA=人名(GBK)
#define CMD_UNKNOWN    0x11   // 有人脸但非注册用户
#define CMD_NOFACE     0x12   // 画面中无人脸
#define CMD_MULTIFACE  0x13   // 画面中有多张人脸
#define CMD_HEARTBEAT  0x1F   // Pi → STM32 心跳（每 5s）
#define CMD_ACK        0x20   // STM32 → Pi 确认回复
```

### 7. 多端协议一致性验证

协议在三个平台上独立实现——C（STM32）、C++（树莓派）、Python（测试）——通过 45 个 pytest 测试验证一致性：

```
tests/protocol/test_frame.py:
  - 编码/解码往返（IDENTIFY/ACK/NOFACE/最大数据）
  - CRC 损坏检测
  - 逐字节解析器（单帧、两帧、垃圾字节、损坏帧）
  - 协议 ID 唯一性
```

## 要学到什么程度

- 理解帧协议设计的核心问题：帧定界、校验、错误恢复
- 理解状态机在协议解析中的作用
- 知道 COBS+CRC 是一种轻量的、适合串口的帧协议方案
- 理解"分层设计"的原则：传输层不关心 CMD 含义，应用层不关心字节怎么传
