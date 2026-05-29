# COBS 编码

## 在这个项目中的作用

COBS（Consistent Overhead Byte Stuffing）是项目中二进制帧协议的基础。它的核心作用是：允许用 `0x00` 作为帧分隔符，同时保证帧数据内部不含 `0x00`。

对应代码：
- Pi 端 C++：`Linux/Model/src/uart_protocol.cpp` — `cobs_encode()` / `cobs_decode()`
- STM32 端 C：`Stm32/Protocol/frame_protocol.c` — `cobs_encode()` / `cobs_decode()`
- Python 测试：`tests/protocol/protocol_ref.py` — `cobs_encode()` / `cobs_decode()`

## 核心原理

### 为什么要 COBS？

二进制数据中可能含有 `0x00` 字节。如果直接用 `0x00` 作为帧分隔符，中间数据的 `0x00` 会被误认为帧边界。COBS 编码消除数据中的 `0x00`，从而可以用 `0x00` 唯一标识帧结束。

### 编码规则

每 254 字节一组，每组前面放一个"计数头"：
- 计数头的值 = 距离下一个 `0x00`（或 255 字节界限）的字节数 + 1
- 遇到 `0x00`：填入当前计数头，开始新组
- 达到 254 个非零字节：计数头填 0xFF，开始新组
- 编码追加的额外字节（Overhead）：至少 1 字节（空数据），最多 `ceil(n/254) + 1` 字节

### 举例

```
原始数据:    [0x00]                      → 编码后: [0x01]
原始数据:    [0x11, 0x22, 0x00, 0x33]   → 编码后: [0x03, 0x11, 0x22, 0x02, 0x33]
原始数据:    [0x11, 0x22, 0x33]          → 编码后: [0x04, 0x11, 0x22, 0x33]
```

### 项目中的实现（STM32 C 版）

```c
// frame_protocol.c — cobs_encode()
static uint16_t cobs_encode(const uint8_t *src, uint16_t src_len, uint8_t *dst) {
    uint8_t *code_ptr = dst++;    // 指向当前计数头
    uint8_t code = 1;             // 当前组已计数的字节数 + 1

    while (src < src_end) {
        if (*src == 0x00) {
            *code_ptr = code;     // 写入计数头
            code = 1;             // 开始新组
            code_ptr = dst++;
            src++;                // 跳过 0x00（不写入输出）
        } else {
            *dst++ = *src++;
            code++;
            if (code == 0xFF) {   // 254 个非零字节了，强制截断
                *code_ptr = code;
                code = 1;
                code_ptr = dst++;
            }
        }
    }
    *code_ptr = code;             // 写入最后一组的计数头
}
```

### 解码规则

顺序读字节：
1. 读计数头 `code`
2. 读出 `code - 1` 个数据字节
3. 如果 `code < 0xFF` 且后面还有字节，说明下一个位置原是 `0x00`，输出一个 `0x00`
4. 如果 `code == 0xFF`，不是 `0x00` 的标记，是 254 个连续非零字节的标记
5. 回到步骤 1

```c
// frame_protocol.c — cobs_decode()
static int cobs_decode(const uint8_t *src, uint16_t src_len, uint8_t *dst, ...) {
    while (src < src_end) {
        uint8_t code = *src++;
        if (code == 0) return -1;  // 无效：计数头不能为 0

        for (i = 1; i < code && src < src_end; i++)
            *dst++ = *src++;        // 复制 code-1 个字节

        if (code < 0xFF && src < src_end)
            *dst++ = 0x00;          // 这个位置原是 0x00
    }
}
```

### COBS 在本项目帧协议中的位置

```
原始帧 = COBS( CMD + DIR + LEN_H + LEN_L + DATA ) + CRC8 + 0x00
        └─────────── COBS 编码区域 ───────────┘
```

0x00 一定只出现在帧末尾作为分隔符，不会出现在中间。

## 要学到什么程度

- 理解 COBS 的动机：二进制数据不能直接用作帧定界
- 能手写一个简单的编码例子（如输入 `00 11 00` → 输出 `01 02 11 01`）
- 理解为什么 COBS 的开销很小（每 254 字节只多 1 字节开销）
- 知道 COBS 相比传统转义（如 SLIP 协议的 `ESC + code`）的优势
