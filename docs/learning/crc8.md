# CRC-8 校验

## 在这个项目中的作用

CRC-8（Cyclic Redundancy Check，8 位循环冗余校验）是帧协议的校验层——发送端对 payload 计算 CRC8，附在帧末尾；接收端重新计算并与收到的 CRC8 对比，不一致则丢弃该帧。

对应代码：
- Pi 端 C++：`Linux/Model/src/uart_protocol.cpp` — `CRC8_TABLE` / `crc8_compute()`
- STM32 端 C：`Stm32/Protocol/frame_protocol.c` — `crc8_table` / `crc8_compute()`
- Python 测试：`tests/protocol/protocol_ref.py` — `CRC8_TABLE` / `crc8_compute()`

## 核心原理

### 多项式与参数

```
多项式: x^8 + x^2 + x + 1  →  0x07
初始值: 0x00
输入不反转、输出不反转、结果不异或
```

### 查表法实现

直接按位计算太慢，256 字节查表法把每个可能的字节值（0x00 - 0xFF）的 CRC 结果预先算好：

```c
// 生成表（预计算）
// CRC8_TABLE[i] = i 经过一次 CRC 迭代后的值
static const uint8_t CRC8_TABLE[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, ...
};
```

项目中三端（C / C++ / Python）使用完全相同的查找表——这是 45 个测试用例 PASS 的前提。

### 计算过程

```c
// frame_protocol.c — crc8_compute()
static uint8_t crc8_compute(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    while (len--)
        crc = CRC8_TABLE[crc ^ *data++];
    return crc;
}
```

每处理一个字节：`crc = table[crc XOR 当前字节]`。最终 crc 值取决于所有输入字节和顺序。

### 在帧中的位置

```
┌──────────────────────────┬──────┬──────┐
│ COBS(payload)            │ CRC8 │ 0x00 │
└──────────────────────────┴──────┴──────┘
                                ↑
                    对原始 payload 计算的校验值
                    计算对象：CMD+DIR+LEN_H+LEN_L+DATA（COBS 解码还原后的原始数据）
```

关键细节：**CRC8 对 COBS 解码后的原始 payload 计算**，不是对 COBS 编码后的数据计算。

### 校验流程

```c
// 接收端解码流程
received_crc = raw[len - 1];                       // 取出发送端附的 CRC
payload = cobs_decode(raw, len - 1);                // 还原原始数据
expected_crc = crc8_compute(payload, payload_len);  // 自己算一遍
if (expected_crc != received_crc) {
    // CRC 不匹配 — 数据在传输中被破坏了，丢弃此帧
    return error;
}
```

### 检错能力

CRC-8 能检测到：
- 任何单比特错误
- 任何奇数个比特错误（因为多项式含 `x+1` 因子）
- 任何长度 ≤ 8 的突发错误
- 约 99.6% 的更长突发错误

注意：CRC 保证**数据完整性**，不保证**安全性**（不防篡改/防伪造）。

## 要学到什么程度

- 理解 CRC 的作用：检测传输过程中的比特错误
- 理解查表法的原理：预计算 → O(1) 查表 vs O(8) 逐位计算
- 知道 0x07 是常见的 CRC-8 多项式
- 理解 CRC 能检测什么、不能检测什么
