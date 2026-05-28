"""UART 通信模块 — COBS+CRC8 二进制帧协议，与 STM32 串口通信。

帧格式 (v2):
  COBS(CMD+DIR+LEN_H+LEN_L+DATA) | CRC8 | 0x00

方向:
  DIR_PI_TO_STM32 = 0x01
  DIR_STM32_TO_PI = 0x02

命令字 (Pi → STM32):
  CMD_IDENTIFY   0x10  识别成功
  CMD_UNKNOWN    0x11  未注册人脸
  CMD_NOFACE     0x12  无人脸
  CMD_MULTIFACE  0x13  多人脸
  CMD_HEARTBEAT  0x1F  心跳 (每 5s)
  CMD_ACK        0x20  确认 (STM32 → Pi)

CRC8 参数: 多项式 0x07, 初始值 0x00, 无反射
"""

from __future__ import annotations

import threading
import time
from collections import deque
from typing import Optional, Dict, Any, Callable

import serial
import serial.tools.list_ports


# ══════════════════════════════════════════════════════════
# 协议常量（与 STM32 frame_protocol.h 一致）
# ══════════════════════════════════════════════════════════

FRAME_DELIMITER = 0x00
CRC8_POLY = 0x07
MAX_DATA_LEN = 64
MAX_FRAME_LEN = MAX_DATA_LEN + 4  # CMD+DIR+LEN_H+LEN_L+DATA
MAX_COBS_LEN = MAX_FRAME_LEN + MAX_FRAME_LEN // 254 + 3

DIR_PI_TO_STM32 = 0x01
DIR_STM32_TO_PI = 0x02

CMD_IDENTIFY = 0x10
CMD_UNKNOWN = 0x11
CMD_NOFACE = 0x12
CMD_MULTIFACE = 0x13
CMD_HEARTBEAT = 0x1F
CMD_ACK = 0x20


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
    """COBS 编码"""
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


def cobs_decode(src: bytes) -> Optional[bytes]:
    """COBS 解码，失败返回 None"""
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
    """编码一帧为 COBS+CRC8+0x00 字节串。

    Args:
        cmd: 命令字 (CMD_IDENTIFY / CMD_UNKNOWN / ...)
        data: 数据载荷 (最多 64 字节)
        dir_: 方向 (默认 Pi→STM32)

    Returns:
        完整帧字节串
    """
    data_len = len(data)
    if data_len > MAX_DATA_LEN:
        raise ValueError(f"数据长度 {data_len} > MAX_DATA_LEN ({MAX_DATA_LEN})")

    payload = bytearray(4 + data_len)
    payload[0] = cmd
    payload[1] = dir_
    payload[2] = (data_len >> 8) & 0xFF
    payload[3] = data_len & 0xFF
    payload[4:] = data

    crc = crc8_compute(bytes(payload))
    cobs_payload = cobs_encode(bytes(payload))

    return cobs_payload + bytes([crc, FRAME_DELIMITER])


def frame_decode(raw: bytes) -> Optional[dict]:
    """解码一帧（不含尾 0x00）。

    Returns:
        {"cmd": int, "dir": int, "data": bytes} 或 None (解码/校验失败)
    """
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

    return {
        "cmd": cmd,
        "dir": dir_,
        "data": bytes(payload[4:]),
    }


# ══════════════════════════════════════════════════════════
# 帧解析器（逐字节喂入，状态机）
# ══════════════════════════════════════════════════════════

class FrameParser:
    """逐字节帧解析器，与 STM32 frame_parser_feed 逻辑一致。"""

    def __init__(self):
        self._buf = bytearray()
        self._state = "WAIT"  # WAIT | DATA

    def reset(self) -> None:
        self._buf.clear()
        self._state = "WAIT"

    def feed(self, byte: int) -> Optional[dict]:
        """喂入一字节，若解析出完整帧则返回 dict，否则返回 None。"""
        if self._state == "WAIT":
            if byte == FRAME_DELIMITER:
                return None
            self._buf.append(byte)
            self._state = "DATA"
            return None

        # STATE_DATA
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
# UART 客户端
# ══════════════════════════════════════════════════════════

class UARTClient:
    """非阻塞 UART 客户端 (COBS+CRC8 帧协议)。

    用法:
        uart = UARTClient(uart_config)
        if uart.open():
            uart.send_frame(CMD_IDENTIFY, b"张三")
        uart.close()
    """

    def __init__(self, uart_config):
        self.cfg = uart_config
        self._ser: Optional[serial.Serial] = None
        self._lock = threading.Lock()

        self._send_queue: deque = deque()
        self._send_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

        # GPIO 状态引脚（可选）
        self._status_pin: Optional[int] = None
        if getattr(self.cfg, "status_pin", None) is not None:
            try:
                import RPi.GPIO as GPIO
                GPIO.setmode(GPIO.BCM)
                GPIO.setup(self.cfg.status_pin, GPIO.IN)
                self._status_pin = self.cfg.status_pin
                print(f"[UART] GPIO 状态引脚: BCM{self.cfg.status_pin}")
            except ImportError:
                print("[UART] 警告: RPi.GPIO 不可用，状态引脚功能禁用")
            except Exception as e:
                print(f"[UART] 警告: GPIO 初始化失败: {e}")

    # ── 连接管理 ──────────────────────────────────────────

    def open(self) -> bool:
        with self._lock:
            if self._ser is not None and self._ser.is_open:
                return True
            try:
                self._ser = serial.Serial(
                    port=self.cfg.device,
                    baudrate=self.cfg.baudrate,
                    timeout=getattr(self.cfg, "timeout", 1.0),
                    write_timeout=1.0,
                )
                print(f"[UART] 已打开: {self.cfg.device} @{self.cfg.baudrate}bps")
                return True
            except serial.SerialException as e:
                print(f"[UART] 错误: 无法打开 {self.cfg.device}: {e}")
                self._ser = None
                return False

    def close(self) -> None:
        with self._lock:
            if self._ser and self._ser.is_open:
                self._ser.close()
                print(f"[UART] 已关闭: {self.cfg.device}")
            self._ser = None

    @property
    def is_open(self) -> bool:
        with self._lock:
            return self._ser is not None and self._ser.is_open

    def reconnect_if_needed(self) -> bool:
        if self.is_open:
            return True
        print(f"[UART] 尝试重连 {self.cfg.device}...")
        return self.open()

    # ── 帧发送 ──────────────────────────────────────────

    def send_frame(self, cmd: int, data: bytes = b"") -> bool:
        """编码并发送一帧。"""
        try:
            frame = frame_encode(cmd, data)
        except ValueError as e:
            print(f"[UART] 帧编码失败: {e}")
            return False
        return self.send_raw(frame)

    def send_raw(self, data: bytes) -> bool:
        """发送原始字节。"""
        with self._lock:
            if not self._ser or not self._ser.is_open:
                print("[UART] 发送失败: 串口未打开")
                return False
            try:
                self._ser.write(data)
                return True
            except serial.SerialException as e:
                print(f"[UART] 发送异常: {e}")
                return False

    def send_heartbeat(self) -> bool:
        return self.send_frame(CMD_HEARTBEAT)

    def send_identify(self, name: str) -> bool:
        return self.send_frame(CMD_IDENTIFY, name.encode("gbk"))

    def send_unknown(self) -> bool:
        return self.send_frame(CMD_UNKNOWN)

    def send_noface(self) -> bool:
        return self.send_frame(CMD_NOFACE)

    def send_multiface(self) -> bool:
        return self.send_frame(CMD_MULTIFACE)

    # ── 接收 ──────────────────────────────────────────────

    def receive_frame(self, timeout: Optional[float] = None) -> Optional[dict]:
        """阻塞读取一帧（基于 FrameParser 状态机）。

        Returns:
            解码后的帧 dict 或 None (超时/错误)。
        """
        parser = FrameParser()
        deadline = None if timeout is None else time.time() + timeout

        while True:
            with self._lock:
                if not self._ser or not self._ser.is_open:
                    return None
                if timeout is not None:
                    remaining = deadline - time.time()
                    if remaining <= 0:
                        self._ser.timeout = 0
                    else:
                        self._ser.timeout = min(remaining, 0.1)
                else:
                    self._ser.timeout = 0.1

                try:
                    byte = self._ser.read(1)
                except serial.SerialException:
                    return None

            if not byte:
                if timeout is not None and time.time() >= deadline:
                    return None
                continue

            result = parser.feed(byte[0])
            if result is not None:
                return result

    def clear_rx_buffer(self) -> None:
        with self._lock:
            if self._ser and self._ser.is_open:
                self._ser.reset_input_buffer()

    # ── GPIO 状态 ──────────────────────────────────────────

    def get_pin_state(self) -> Optional[bool]:
        if self._status_pin is None:
            return None
        try:
            import RPi.GPIO as GPIO
            return bool(GPIO.input(self._status_pin))
        except Exception:
            return None

    def health(self) -> Dict[str, Any]:
        return {
            "connected": self.is_open,
            "device": self.cfg.device,
            "baudrate": self.cfg.baudrate,
            "status_pin": self.get_pin_state(),
        }

    # ── 工具 ──────────────────────────────────────────────

    @staticmethod
    def list_ports() -> list:
        ports = serial.tools.list_ports.comports()
        return [{"device": p.device, "name": p.name,
                 "description": p.description, "hwid": p.hwid} for p in ports]


# ══════════════════════════════════════════════════════════
# 结果处理线程：ResultQueue → UART 帧发送
# ══════════════════════════════════════════════════════════

def uart_control_loop(
    result_q,
    uart: UARTClient,
    stop_event: threading.Event,
    heartbeat_interval: float = 5.0,
) -> None:
    """UART 控制循环（独立线程）。

    监听 ResultQueue → 发送二进制帧给 STM32。
    同时维护心跳定时器。
    """
    print("[UART-Ctrl] 控制线程启动 (COBS+CRC8 帧协议)")
    last_heartbeat = time.time()

    while not stop_event.is_set():
        # ── 心跳 ──
        now = time.time()
        if now - last_heartbeat >= heartbeat_interval:
            if uart.reconnect_if_needed():
                uart.send_heartbeat()
            last_heartbeat = now

        # ── 取推理结果 ──
        result = result_q.pop(timeout=0.5)
        if result is None:
            continue

        if result.get("type") != "identify":
            continue

        name = result.get("name", "?")
        confidence = result.get("confidence", 0.0)

        if not uart.reconnect_if_needed():
            print("[UART-Ctrl] 串口未连接，跳过")
            continue

        if name != "?":
            print(f"[UART-Ctrl] 识别到 {name} (置信度={confidence:.3f}) → CMD_IDENTIFY")
            uart.send_identify(name)
        else:
            print(f"[UART-Ctrl] 未识别 (最高={confidence:.3f}) → CMD_UNKNOWN")
            uart.send_unknown()

    print("[UART-Ctrl] 控制线程退出")


# ══════════════════════════════════════════════════════════
# 接收监听线程
# ══════════════════════════════════════════════════════════

def uart_receive_monitor(
    uart: UARTClient,
    stop_event: threading.Event,
    on_message: Optional[Callable[[dict], None]] = None,
) -> None:
    """UART 接收监听线程。持续解码帧，可选回调 on_message(frame_dict)。"""
    print("[UART-Mon] 接收监听线程启动")

    while not stop_event.is_set():
        if not uart.is_open:
            uart.reconnect_if_needed()
            stop_event.wait(1.0)
            continue

        frame = uart.receive_frame(timeout=1.0)
        if frame is None:
            continue

        cmd = frame["cmd"]
        cmd_names = {0x20: "ACK", 0x1F: "HEARTBEAT"}
        cmd_name = cmd_names.get(cmd, f"0x{cmd:02X}")
        print(f"[UART-Mon] ← STM32: {cmd_name}")

        if on_message:
            try:
                on_message(frame)
            except Exception as e:
                print(f"[UART-Mon] 回调异常: {e}")

    print("[UART-Mon] 接收监听线程退出")
