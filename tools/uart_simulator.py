"""UART 人脸识别模拟器 — 模拟摄像头推理结果发送给 STM32。

用法:
    python3 uart_simulator.py [--port /dev/serial0] [--baud 115200]

交互按键:
    i <姓名>  → CMD_IDENTIFY   (识别成功，姓名用 GBK 编码)
    u         → CMD_UNKNOWN    (有人脸但未注册)
    n         → CMD_NOFACE     (无人脸)
    m         → CMD_MULTIFACE  (多人脸)
    h         → CMD_HEARTBEAT  (心跳)
    a         → 自动模式 (循环模拟识别流程)
    s         → 停止自动模式
    q         → 退出

帧格式: COBS(CMD+DIR+LEN_H+LEN_L+DATA) | CRC8 | 0x00
"""

from __future__ import annotations

import argparse
import struct
import sys
import threading
import time

# ── 尝试导入 serial ──
try:
    import serial
except ImportError:
    print("需要 pyserial，请先安装: pip3 install pyserial")
    sys.exit(1)


# ══════════════════════════════════════════════════════════
# 协议常量（与 STM32 frame_protocol.h 一致）
# ══════════════════════════════════════════════════════════

FRAME_DELIMITER = 0x00
MAX_DATA_LEN = 64
MAX_FRAME_LEN = MAX_DATA_LEN + 4
MAX_COBS_LEN = MAX_FRAME_LEN + MAX_FRAME_LEN // 254 + 3

DIR_PI_TO_STM32 = 0x01
DIR_STM32_TO_PI = 0x02

CMD_IDENTIFY   = 0x10
CMD_UNKNOWN    = 0x11
CMD_NOFACE     = 0x12
CMD_MULTIFACE  = 0x13
CMD_HEARTBEAT  = 0x1F
CMD_ACK        = 0x20

CMD_NAMES = {
    0x10: "IDENTIFY",
    0x11: "UNKNOWN",
    0x12: "NOFACE",
    0x13: "MULTIFACE",
    0x1F: "HEARTBEAT",
    0x20: "ACK",
}


# ══════════════════════════════════════════════════════════
# CRC8 查表 (多项式 0x07, 初始值 0x00, 无反射)
# ══════════════════════════════════════════════════════════

CRC8_TABLE = (
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
)


def crc8_compute(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc = CRC8_TABLE[crc ^ b]
    return crc


# ══════════════════════════════════════════════════════════
# COBS 编解码
# ══════════════════════════════════════════════════════════

def cobs_encode(src: bytes) -> bytes:
    if not src:
        return b"\x01"
    result = bytearray()
    code_ptr = 0
    code = 1
    result.append(0)
    for b in src:
        if b == 0x00:
            result[code_ptr] = code
            code = 1
            code_ptr = len(result)
            result.append(0)
        else:
            result.append(b)
            code += 1
            if code == 0xFF:
                result[code_ptr] = code
                code = 1
                code_ptr = len(result)
                result.append(0)
    result[code_ptr] = code
    return bytes(result)


def cobs_decode(src: bytes):
    result = bytearray()
    i = 0
    while i < len(src):
        code = src[i]
        i += 1
        if code == 0:
            return None
        for _ in range(1, code):
            if i >= len(src):
                return None
            result.append(src[i])
            i += 1
        if code < 0xFF and i < len(src):
            result.append(0x00)
    return bytes(result)


# ══════════════════════════════════════════════════════════
# 帧编解码
# ══════════════════════════════════════════════════════════

def frame_encode(cmd: int, data: bytes = b"", dir_: int = DIR_PI_TO_STM32) -> bytes:
    data_len = len(data)
    if data_len > MAX_DATA_LEN:
        raise ValueError(f"数据长度 {data_len} > {MAX_DATA_LEN}")

    payload = bytearray(4 + data_len)
    payload[0] = cmd
    payload[1] = dir_
    payload[2] = (data_len >> 8) & 0xFF
    payload[3] = data_len & 0xFF
    payload[4:] = data

    crc = crc8_compute(bytes(payload))
    cobs_payload = cobs_encode(bytes(payload))
    return cobs_payload + bytes([crc, FRAME_DELIMITER])


def frame_decode(raw: bytes):
    if len(raw) < 2:
        return None

    received_crc = raw[-1]
    cobs_data = raw[:-1]

    payload = cobs_decode(cobs_data)
    if payload is None:
        return None
    if len(payload) < 4 or len(payload) > MAX_DATA_LEN + 4:
        return None

    expected_crc = crc8_compute(payload)
    if expected_crc != received_crc:
        return None

    cmd = payload[0]
    dir_ = payload[1]
    data_len = (payload[2] << 8) | payload[3]
    if data_len != len(payload) - 4:
        return None

    return {"cmd": cmd, "dir": dir_, "data": bytes(payload[4:])}


# ══════════════════════════════════════════════════════════
# 帧解析器（逐字节状态机）
# ══════════════════════════════════════════════════════════

class FrameParser:
    def __init__(self):
        self._buf = bytearray()
        self._state = "WAIT"

    def reset(self):
        self._buf.clear()
        self._state = "WAIT"

    def feed(self, byte: int):
        if self._state == "WAIT":
            if byte == FRAME_DELIMITER:
                return None
            self._buf.append(byte)
            self._state = "DATA"
            return None

        if len(self._buf) >= MAX_COBS_LEN:
            self.reset()
            return None

        if byte == FRAME_DELIMITER:
            result = frame_decode(bytes(self._buf))
            self.reset()
            return result

        self._buf.append(byte)
        return None


# ══════════════════════════════════════════════════════════
# UART 接收线程
# ══════════════════════════════════════════════════════════

def rx_thread(ser: serial.Serial, stop_event: threading.Event):
    """后台接收 STM32 的 ACK 帧并打印。"""
    parser = FrameParser()
    ser.timeout = 0.2
    while not stop_event.is_set():
        try:
            b = ser.read(1)
        except serial.SerialException:
            break
        if not b:
            continue
        frame = parser.feed(b[0])
        if frame:
            cmd_name = CMD_NAMES.get(frame["cmd"], f"0x{frame['cmd']:02X}")
            data_str = ""
            if frame["data"]:
                try:
                    data_str = frame["data"].decode("utf-8", errors="replace")
                except Exception:
                    data_str = frame["data"].hex()
            print(f"  ← STM32: {cmd_name}  data={data_str}")


# ══════════════════════════════════════════════════════════
# 主逻辑
# ══════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="UART 人脸识别模拟器")
    parser.add_argument("--port", default="/dev/serial0", help="串口设备 (默认 /dev/serial0)")
    parser.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    args = parser.parse_args()

    print(f"打开串口 {args.port} @ {args.baud} bps ...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1.0, write_timeout=1.0)
    except serial.SerialException as e:
        print(f"无法打开串口: {e}")
        sys.exit(1)

    print(f"已连接。\n")

    stop_rx = threading.Event()
    rx = threading.Thread(target=rx_thread, args=(ser, stop_rx), daemon=True)
    rx.start()

    auto_mode = False
    auto_stop = threading.Event()

    def auto_loop():
        """自动模式: 循环模拟 识别→无人→多人→未注册"""
        seq = [
            (CMD_IDENTIFY, "张三"),
            (CMD_NOFACE, ""),
            (CMD_IDENTIFY, "李四"),
            (CMD_UNKNOWN, ""),
            (CMD_MULTIFACE, ""),
            (CMD_IDENTIFY, "王五"),
        ]
        idx = 0
        while not auto_stop.is_set():
            cmd, name = seq[idx % len(seq)]
            data = name.encode("gbk") if name else b""
            frame = frame_encode(cmd, data)
            ser.write(frame)
            cmd_name = CMD_NAMES[cmd]
            print(f"  → STM32: {cmd_name}  name={name}")
            idx += 1
            time.sleep(3)

    print("=" * 54)
    print("  UART 人脸识别模拟器 — 协议: COBS+CRC8")
    print("=" * 54)
    print("  i <姓名>   识别成功  (例: i 张三)")
    print("  u          未注册人脸")
    print("  n          无人脸")
    print("  m          多人脸")
    print("  h          心跳")
    print("  a          自动模式（循环模拟）")
    print("  s          停止自动模式")
    print("  q          退出")
    print("=" * 54 + "\n")

    try:
        while True:
            user_input = input("> ").strip()
            if not user_input:
                continue

            parts = user_input.split(maxsplit=1)
            action = parts[0].lower()

            if action == "q":
                break
            elif action == "a":
                if not auto_mode:
                    auto_mode = True
                    auto_stop.clear()
                    t = threading.Thread(target=auto_loop, daemon=True)
                    t.start()
                    print("[自动模式] 已启动，每 3s 发送一帧")
            elif action == "s":
                if auto_mode:
                    auto_stop.set()
                    auto_mode = False
                    print("[自动模式] 已停止")
            elif action == "h":
                frame = frame_encode(CMD_HEARTBEAT)
                ser.write(frame)
                print("  → STM32: HEARTBEAT")
            elif action == "i":
                name = parts[1] if len(parts) > 1 else "测试"
                data = name.encode("gbk")
                frame = frame_encode(CMD_IDENTIFY, data)
                ser.write(frame)
                print(f"  → STM32: IDENTIFY  name={name}")
            elif action == "u":
                frame = frame_encode(CMD_UNKNOWN)
                ser.write(frame)
                print("  → STM32: UNKNOWN")
            elif action == "n":
                frame = frame_encode(CMD_NOFACE)
                ser.write(frame)
                print("  → STM32: NOFACE")
            elif action == "m":
                frame = frame_encode(CMD_MULTIFACE)
                ser.write(frame)
                print("  → STM32: MULTIFACE")
            else:
                print(f"  未知命令: {action}")

    except KeyboardInterrupt:
        print("\n中断")
    finally:
        auto_stop.set()
        stop_rx.set()
        ser.close()
        print("串口已关闭。")


if __name__ == "__main__":
    main()
