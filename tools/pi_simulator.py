"""
pi_simulator.py — PC 端模拟树莓派，通过 USB-TTL 与 STM32 通信

帧协议（与 Stm32/User/main.c 完全一致）：
  帧头   CMD    DIR    LEN_H  LEN_L  DATA(转义)  XOR   帧尾
  0xAA   1B     1B     1B     1B     N bytes     1B    0x55

DIR: 0x01=Pi→STM32, 0x02=STM32→Pi
CMD: 0x10=IDENTIFY, 0x11=UNKNOWN, 0x12=NOFACE, 0x13=MULTIFACE, 0x1F=HEARTBEAT, 0x20=ACK

用法：
  python pi_simulator.py COM3          # 交互模式
  python pi_simulator.py COM3 --auto   # 自动测试模式
"""

import serial
import time
import sys
import struct

# ── 协议常量 ──
FRAME_HEADER    = 0xAA
FRAME_TAIL      = 0x55
ESCAPE_BYTE     = 0xBB
ESCAPE_XOR_A    = 0x55   # 0xBB 0x55 → 0xAA
ESCAPE_XOR_B    = 0xAA   # 0xBB 0xAA → 0x55
ESCAPE_XOR_SELF = 0x44   # 0xBB 0x44 → 0xBB

DIR_PI_TO_STM32  = 0x01
DIR_STM32_TO_PI  = 0x02

CMD_IDENTIFY  = 0x10
CMD_UNKNOWN   = 0x11
CMD_NOFACE    = 0x12
CMD_MULTIFACE = 0x13
CMD_HEARTBEAT = 0x1F
CMD_ACK       = 0x20

CMD_NAMES = {
    0x10: "IDENTIFY",
    0x11: "UNKNOWN",
    0x12: "NOFACE",
    0x13: "MULTIFACE",
    0x1F: "HEARTBEAT",
    0x20: "ACK",
}


def encode_frame(cmd: int, dir_: int, data: bytes) -> bytes:
    """组帧 + 转义编码，与 main.c frame_encode() 一致"""
    buf = bytearray()
    buf.append(FRAME_HEADER)
    buf.append(cmd)
    buf.append(dir_)
    data_len = len(data)
    buf.append((data_len >> 8) & 0xFF)
    buf.append(data_len & 0xFF)

    # DATA 区转义
    for b in data:
        if b == FRAME_HEADER:
            buf.extend([ESCAPE_BYTE, ESCAPE_XOR_A])
        elif b == FRAME_TAIL:
            buf.extend([ESCAPE_BYTE, ESCAPE_XOR_B])
        elif b == ESCAPE_BYTE:
            buf.extend([ESCAPE_BYTE, ESCAPE_XOR_SELF])
        else:
            buf.append(b)

    # XOR: CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0..N-1]
    xor_val = cmd ^ dir_ ^ ((data_len >> 8) & 0xFF) ^ (data_len & 0xFF)
    for b in data:
        xor_val ^= b
    buf.append(xor_val)
    buf.append(FRAME_TAIL)

    return bytes(buf)


def decode_frame(raw: bytes) -> tuple | None:
    """解码 STM32 回复帧，返回 (cmd, data_bytes) 或 None"""
    buf = bytearray()
    esc_state = False

    # 找帧头
    i = 0
    while i < len(raw) and raw[i] != FRAME_HEADER:
        i += 1
    if i >= len(raw):
        return None
    i += 1  # 跳过帧头

    # 读 CMD
    if i >= len(raw): return None
    cmd = raw[i]; i += 1

    # 读 DIR
    if i >= len(raw): return None
    dir_ = raw[i]; i += 1

    # 读 LEN
    if i + 1 >= len(raw): return None
    data_len = (raw[i] << 8) | raw[i + 1]
    i += 2

    # 读 DATA（反转义）
    while data_len > 0 and i < len(raw):
        b = raw[i]; i += 1
        if esc_state:
            esc_state = False
            if b == ESCAPE_XOR_A:
                buf.append(FRAME_HEADER)
            elif b == ESCAPE_XOR_B:
                buf.append(FRAME_TAIL)
            elif b == ESCAPE_XOR_SELF:
                buf.append(ESCAPE_BYTE)
            data_len -= 1
        elif b == ESCAPE_BYTE:
            esc_state = True
        else:
            buf.append(b)
            data_len -= 1

    data = bytes(buf)

    # XOR
    if i >= len(raw): return None
    xor_rx = raw[i]; i += 1

    # 帧尾
    if i >= len(raw) or raw[i] != FRAME_TAIL:
        return None

    # 校验 XOR
    xor_calc = cmd ^ dir_
    xor_calc ^= ((len(data) >> 8) & 0xFF) ^ (len(data) & 0xFF)
    for b in data:
        xor_calc ^= b
    if xor_rx != xor_calc:
        print(f"  [警告] XOR 校验失败: recv={xor_rx:02X} calc={xor_calc:02X}")

    return (cmd, data)


def send_and_recv(ser: serial.Serial, cmd: int, data: bytes = b"", timeout: float = 1.0):
    """发送一帧并等待回复"""
    frame = encode_frame(cmd, DIR_PI_TO_STM32, data)
    print(f"  → 发送 {CMD_NAMES.get(cmd, f'0x{cmd:02X}')}: {frame.hex(' ')}")
    ser.write(frame)

    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting:
            raw = ser.read(ser.in_waiting)
            result = decode_frame(raw)
            if result:
                rcmd, rdata = result
                name = CMD_NAMES.get(rcmd, f"0x{rcmd:02X}")
                print(f"  ← 收到 {name}: {rdata.decode('utf-8', errors='replace') if rdata else '(空)'}")
                return result
        time.sleep(0.01)
    print("  (超时无回复)")
    return None


def interactive(ser: serial.Serial):
    """交互模式"""
    print("=" * 60)
    print("  STM32 帧协议测试 — 交互模式")
    print("  命令: h=心跳 i=识别 u=陌生人 n=无人脸 m=多人脸 q=退出")
    print("=" * 60)
    while True:
        try:
            ch = input("\n> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break

        if ch == 'q':
            break
        elif ch == 'h':
            send_and_recv(ser, CMD_HEARTBEAT)
        elif ch == 'i':
            name = input("  识别名称(默认'张三'): ").strip() or "张三"
            conf = input("  置信度(默认'0.95'): ").strip() or "0.95"
            uid  = input("  用户ID(默认'001'): ").strip() or "001"
            data = f"{uid},{name},{conf}".encode()
            send_and_recv(ser, CMD_IDENTIFY, data)
        elif ch == 'u':
            send_and_recv(ser, CMD_UNKNOWN)
        elif ch == 'n':
            send_and_recv(ser, CMD_NOFACE)
        elif ch == 'm':
            send_and_recv(ser, CMD_MULTIFACE)
        else:
            print("  未知命令。h/i/u/n/m/q")


def auto_test(ser: serial.Serial):
    """自动测试：依次发所有命令"""
    print("=" * 60)
    print("  STM32 帧协议测试 — 自动模式")
    print("=" * 60)

    tests = [
        ("心跳",      CMD_HEARTBEAT, b""),
        ("无人脸",    CMD_NOFACE,    b""),
        ("陌生人",    CMD_UNKNOWN,   b""),
        ("多人脸",    CMD_MULTIFACE, b""),
        ("识别-张三", CMD_IDENTIFY,  b"001,\xe5\xbc\xa0\xe4\xb8\x89,0.95"),  # UTF-8 "张三"
    ]

    for label, cmd, data in tests:
        print(f"\n[{label}]")
        send_and_recv(ser, cmd, data)
        time.sleep(0.5)

    # 心跳 3 连发
    print("\n[心跳 x3 (1秒间隔)]")
    for i in range(3):
        send_and_recv(ser, CMD_HEARTBEAT)
        time.sleep(1.0)

    print("\n" + "=" * 60)
    print("  测试完毕。观察 STM32 LED:")
    print("    识别成功 → 绿灯常亮")
    print("    陌生人   → LED 快闪 3 次")
    print("    无人脸   → LED 熄灭")
    print("    多人脸   → LED 快速闪烁")
    print("    心跳     → LED 短闪一次")
    print("    15秒无帧 → LED 慢闪(心跳超时告警)")
    print("=" * 60)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python pi_simulator.py <COM口> [--auto]")
        print("示例: python pi_simulator.py COM3")
        print("      python pi_simulator.py COM3 --auto")
        sys.exit(1)

    port = sys.argv[1]
    auto = "--auto" in sys.argv

    ser = serial.Serial(port=port, baudrate=115200, bytesize=8,
                        parity='N', stopbits=1, timeout=0.1)
    print(f"已连接 {port} @ 115200-8-N-1")
    time.sleep(0.2)

    try:
        if auto:
            auto_test(ser)
        else:
            interactive(ser)
    finally:
        ser.close()
        print("\n串口已关闭。")
