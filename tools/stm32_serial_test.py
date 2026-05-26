#!/usr/bin/env python3
"""
STM32 COBS+CRC8 帧协议测试工具

用法：
  # 列出可用串口
  python stm32_serial_test.py --list

  # 发送心跳帧（正常）
  python stm32_serial_test.py --port COM3 --baud 115200 --cmd heartbeat

  # 发送识别成功帧（正常）
  python stm32_serial_test.py --port COM3 --cmd identify

  # 场景1: CRC8 损坏
  python stm32_serial_test.py --port COM3 --cmd corrupt-crc

  # 场景2: 长度与实际数据不符
  python stm32_serial_test.py --port COM3 --cmd mismatch-len

  # 场景3: LEN 超过 MAX_DATA_LEN (64)
  python stm32_serial_test.py --port COM3 --cmd over-max

  # 场景4: 帧中间停顿 >5ms
  python stm32_serial_test.py --port COM3 --cmd gap-timeout

  # 交互模式（手动输入十六进制）
  python stm32_serial_test.py --port COM3 --interactive

  # 只编码不发送（调试用）
  python stm32_serial_test.py --cmd heartbeat --dry-run

连接方式：
  USB-TTL  TX  ────  STM32 PA10 (USART1 RX)
  USB-TTL  RX  ────  STM32 PA9  (USART1 TX)
  USB-TTL  GND ────  STM32 GND
"""

import sys
import os
import time
import argparse
import struct

# ═══════════════════════════════════════════════════════════
#  协议常量
# ═══════════════════════════════════════════════════════════

FRAME_DELIMITER = 0x00
CRC8_POLY = 0x07
MAX_DATA_LEN = 64
MAX_FRAME_LEN = MAX_DATA_LEN + 4

# 方向
DIR_PI_TO_STM32 = 0x01
DIR_STM32_TO_PI = 0x02

# 命令字
CMD = {
    "identify":  0x10,
    "unknown":   0x11,
    "noface":    0x12,
    "multiface": 0x13,
    "heartbeat": 0x1F,
    "ack":       0x20,
}

CMD_NAME = {v: k for k, v in CMD.items()}

# ═══════════════════════════════════════════════════════════
#  CRC8 查表 (多项式 0x07, 初始值 0x00)
# ═══════════════════════════════════════════════════════════

CRC8_TABLE = [
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
]


def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc = CRC8_TABLE[crc ^ b]
    return crc


# ═══════════════════════════════════════════════════════════
#  COBS 编码
# ═══════════════════════════════════════════════════════════

def cobs_encode(data: bytes) -> bytes:
    result = bytearray()
    code = 1
    block = bytearray()
    for b in data:
        if b == 0x00:
            result.append(code)
            result.extend(block)
            code = 1
            block = bytearray()
        else:
            block.append(b)
            code += 1
            if code == 0xFF:
                result.append(code)
                result.extend(block)
                code = 1
                block = bytearray()
    result.append(code)
    result.extend(block)
    return bytes(result)


def cobs_decode(data: bytes) -> bytes:
    result = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        for _ in range(1, code):
            if i >= len(data):
                break
            result.append(data[i])
            i += 1
        if code < 0xFF and i < len(data):
            result.append(0x00)
    return bytes(result)


# ═══════════════════════════════════════════════════════════
#  帧编解码
# ═══════════════════════════════════════════════════════════

def build_frame(cmd: int, data: bytes = b"") -> bytes:
    """构建 COBS+CRC8 帧"""
    dir_ = DIR_PI_TO_STM32
    data_len = len(data)
    if data_len > MAX_DATA_LEN:
        raise ValueError(f"Data too long: {data_len} > {MAX_DATA_LEN}")

    # payload = CMD(1B) + DIR(1B) + LEN_H+LEN_L(2B big-endian) + DATA
    payload = struct.pack(">BBH", cmd, dir_, data_len) + data
    cobs_payload = cobs_encode(payload)
    crc = crc8(payload)
    return cobs_payload + bytes([crc, FRAME_DELIMITER])


def build_payload_manual(cmd: int, dir_: int, data: bytes) -> bytes:
    """手动构建 payload（用于测试）"""
    data_len = len(data)
    return struct.pack(">BBH", cmd, dir_, data_len) + data


def build_frame_from_payload(payload: bytes) -> bytes:
    """从原始 payload 构建 COBS+CRC8 帧"""
    cobs_payload = cobs_encode(payload)
    crc = crc8(payload)
    return cobs_payload + bytes([crc, FRAME_DELIMITER])


def decode_frame(data: bytes) -> tuple:
    """解码帧，返回 (cmd, dir, data) 或 None"""
    if len(data) < 3:
        return None
    if data[-1] != FRAME_DELIMITER:
        return None
    received_crc = data[-2]
    cobs_data = data[:-2]
    try:
        payload = cobs_decode(cobs_data)
    except Exception:
        return None
    if len(payload) < 4:
        return None
    if crc8(payload) != received_crc:
        return None
    cmd = payload[0]
    dir_ = payload[1]
    data_len = struct.unpack(">H", payload[2:4])[0]
    actual_data = payload[4:]
    if data_len != len(actual_data):
        return None
    return cmd, dir_, actual_data


# ═══════════════════════════════════════════════════════════
#  测试帧生成
# ═══════════════════════════════════════════════════════════

def frame_heartbeat():
    """心跳帧：CMD=0x1F, 无数据"""
    return build_frame(CMD["heartbeat"])


def frame_identify():
    """识别成功帧：CMD=0x10, 无数据"""
    return build_frame(CMD["identify"])


def frame_unknown():
    """未识别帧：CMD=0x11, 无数据"""
    return build_frame(CMD["unknown"])


def frame_corrupt_crc():
    """场景1: CRC8 损坏 — 正确帧但 CRC8 字节异或 0xFF"""
    frame = build_frame(CMD["heartbeat"])
    frame = bytearray(frame)
    frame[-2] ^= 0xFF  # 篡改 CRC8
    return bytes(frame)


def frame_mismatch_len():
    """场景2: LEN 与实际 DATA 不符 — 宣称 LEN=10 但只有 3 字节"""
    payload = struct.pack(">BBH", CMD["heartbeat"], DIR_PI_TO_STM32, 10)
    payload += b"\x01\x02\x03"  # 实际只有 3 字节
    return build_frame_from_payload(payload)


def frame_over_max():
    """场景3: LEN 超过 MAX_DATA_LEN (64) — 发 LEN=256"""
    payload = struct.pack(">BBH", CMD["heartbeat"], DIR_PI_TO_STM32, 256)
    payload += b"\x00" * 64  # 不够也没关系，STM32 应检查 LEN 就丢弃
    return build_frame_from_payload(payload)


def frame_gap_timeout():
    """场景4: 字节间停顿 >5ms — 用于逐字节手动发送"""
    return build_frame(CMD["heartbeat"])


# ═══════════════════════════════════════════════════════════
#  串口通信
# ═══════════════════════════════════════════════════════════

def list_ports():
    try:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("No serial ports found")
            return
        for p in ports:
            print(f"  {p.device} - {p.description}")
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial")
        return


def open_serial(port, baud=115200):
    import serial
    ser = serial.Serial(port, baud, timeout=1)
    print(f"[Serial] Connected: {port} @ {baud}")
    return ser


def send_frame(ser, frame: bytes):
    """发送整帧"""
    hex_str = " ".join(f"{b:02X}" for b in frame)
    print(f"[TX] {len(frame)} bytes: {hex_str}")
    ser.write(frame)
    ser.flush()


def send_frame_with_gap(ser, frame: bytes, gap_ms=6):
    """场景4: 在帧中间插入停顿"""
    import serial
    split = len(frame) // 2
    part1 = frame[:split]
    part2 = frame[split:]

    hex1 = " ".join(f"{b:02X}" for b in part1)
    print(f"[TX-gap] Part1 {len(part1)} bytes: {hex1}")
    ser.write(part1)
    ser.flush()

    print(f"[TX-gap] --- sleeping {gap_ms}ms ---")
    time.sleep(gap_ms / 1000.0)

    hex2 = " ".join(f"{b:02X}" for b in part2)
    print(f"[TX-gap] Part2 {len(part2)} bytes: {hex2}")
    ser.write(part2)
    ser.flush()


def read_response(ser, timeout=1.0):
    """读取 STM32 响应"""
    import serial
    start = time.time()
    data = bytearray()
    while time.time() - start < timeout:
        if ser.in_waiting:
            b = ser.read(ser.in_waiting)
            data.extend(b)
        else:
            time.sleep(0.01)
    if data:
        try:
            text = data.decode("ascii", errors="replace").strip()
            if text:
                print(f"[RX-text] {text}")
        except Exception:
            pass
        hex_str = " ".join(f"{b:02X}" for b in data)
        print(f"[RX-hex] {len(data)} bytes: {hex_str}")

        # 尝试解码帧
        result = decode_frame(bytes(data))
        if result:
            cmd, dir_, payload = result
            name = CMD_NAME.get(cmd, f"0x{cmd:02X}")
            print(f"[RX-frame] CMD={name}(0x{cmd:02X}) DIR=0x{dir_:02X} DATA={payload.hex() if payload else '(empty)'}")
    else:
        print("[RX] (no response)")


# ═══════════════════════════════════════════════════════════
#  交互模式
# ═══════════════════════════════════════════════════════════

def interactive_mode(ser):
    print("""
╔══════════════════════════════════════════════════╗
║  STM32 帧协议测试 - 交互模式                      ║
║                                                  ║
║  命令:                                            ║
║    hb              发送心跳帧                      ║
║    id              发送识别成功帧                   ║
║    uk              发送未识别帧                     ║
║    crc             发送 CRC8 损坏帧                 ║
║    len             发送 LEN 不匹配帧                ║
║    max             发送 LEN 超范围帧                ║
║    gap             发送帧间停顿帧                   ║
║    hex AA BB CC... 发送自定义十六进制帧              ║
║    raw AA BB CC... 发送原始十六进制（不加帧协议）    ║
║    q               退出                            ║
╚══════════════════════════════════════════════════╝
""")

    test_frames = {
        "hb":  ("心跳", frame_heartbeat),
        "id":  ("识别成功", frame_identify),
        "uk":  ("未识别", frame_unknown),
        "crc": ("CRC8损坏", frame_corrupt_crc),
        "len": ("LEN不匹配", frame_mismatch_len),
        "max": ("LEN超范围", frame_over_max),
    }

    while True:
        try:
            line = input("\n> ").strip()
        except (EOFError, KeyboardInterrupt):
            break

        if not line:
            continue
        if line == "q":
            break

        if line == "gap":
            frame = frame_gap_timeout()
            print(f"[gap] Sending heartbeat with mid-frame {6}ms gap...")
            send_frame_with_gap(ser, frame, gap_ms=6)
            read_response(ser)
            continue

        if line in test_frames:
            name, fn = test_frames[line]
            frame = fn()
            hex_str = " ".join(f"{b:02X}" for b in frame)
            print(f"[{name}] {hex_str}")
            send_frame(ser, frame)
            read_response(ser)
            continue

        if line.startswith("hex "):
            hex_part = line[4:].strip()
            try:
                raw = bytes.fromhex(hex_part)
                frame = build_frame_from_payload(raw) if raw[-1] != 0x00 else raw
                send_frame(ser, frame)
                read_response(ser)
            except ValueError as e:
                print(f"Invalid hex: {e}")
            continue

        if line.startswith("raw "):
            hex_part = line[4:].strip()
            try:
                raw = bytes.fromhex(hex_part)
                hex_str = " ".join(f"{b:02X}" for b in raw)
                print(f"[raw] {len(raw)} bytes: {hex_str}")
                ser.write(raw)
                ser.flush()
                read_response(ser)
            except ValueError as e:
                print(f"Invalid hex: {e}")
            continue

        print(f"Unknown command: {line}")


# ═══════════════════════════════════════════════════════════
#  入口
# ═══════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="STM32 COBS+CRC8 Frame Protocol Test Tool")
    parser.add_argument("--port", default="COM3", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--list", action="store_true", help="List available serial ports")
    parser.add_argument("--dry-run", action="store_true", help="Encode only, don't send")
    parser.add_argument("--cmd", choices=[
        "heartbeat", "identify", "unknown", "noface", "multiface",
        "corrupt-crc", "mismatch-len", "over-max", "gap-timeout",
    ], help="Test command to send")
    parser.add_argument("--data", help="Hex data payload (e.g. '010203')")
    parser.add_argument("--interactive", "-i", action="store_true", help="Interactive mode")
    args = parser.parse_args()

    if args.list:
        list_ports()
        return

    if args.dry_run:
        cmd_map = {
            "heartbeat": frame_heartbeat,
            "identify": frame_identify,
            "unknown": frame_unknown,
            "corrupt-crc": frame_corrupt_crc,
            "mismatch-len": frame_mismatch_len,
            "over-max": frame_over_max,
            "gap-timeout": frame_gap_timeout,
        }
        if args.cmd in cmd_map:
            frame = cmd_map[args.cmd]()
            print(f"[dry-run] {args.cmd}: {' '.join(f'{b:02X}' for b in frame)}")
        return

    if args.interactive or not args.cmd:
        if args.interactive:
            ser = open_serial(args.port, args.baud)
            try:
                interactive_mode(ser)
            finally:
                ser.close()
                print("[Serial] Closed")
        else:
            if not args.cmd:
                print("Nothing to do. Use --cmd or --interactive")
            return
    else:
        ser = open_serial(args.port, args.baud)
        try:
            cmd_map = {
                "heartbeat": frame_heartbeat,
                "identify": frame_identify,
                "unknown": frame_unknown,
                "corrupt-crc": frame_corrupt_crc,
                "mismatch-len": frame_mismatch_len,
                "over-max": frame_over_max,
                "gap-timeout": frame_gap_timeout,
            }
            frame = cmd_map[args.cmd]()
            if args.cmd == "gap-timeout":
                send_frame_with_gap(ser, frame, gap_ms=6)
            else:
                send_frame(ser, frame)
            read_response(ser)
        finally:
            ser.close()
            print("[Serial] Closed")


if __name__ == "__main__":
    main()
