"""Reference implementation of COBS + CRC8 + frame encode/decode.

Pure-Python, zero external dependencies.  Algorithm is identical to:
  - Stm32/Protocol/frame_protocol.c  (C)
  - Linux/Raspberry Pi/src/uart.py   (Python)

This module exists so tests can import protocol logic without pulling in
serial, RPi.GPIO, or any other runtime dependency.
"""

# ── Protocol constants (must match frame_protocol.h) ──────────────

FRAME_DELIMITER = 0x00
MAX_DATA_LEN = 64
MAX_FRAME_LEN = MAX_DATA_LEN + 4          # CMD + DIR + LEN_H + LEN_L + DATA
MAX_COBS_LEN  = MAX_FRAME_LEN + MAX_FRAME_LEN // 254 + 3

DIR_PI_TO_STM32  = 0x01
DIR_STM32_TO_PI  = 0x02

CMD_IDENTIFY   = 0x10
CMD_UNKNOWN    = 0x11
CMD_NOFACE     = 0x12
CMD_MULTIFACE  = 0x13
CMD_HEARTBEAT  = 0x1F
CMD_ACK        = 0x20
CMD_DELETE     = 0x21  # Pi → STM32: remote delete notification

# ── CRC8 table (poly 0x07, init 0x00, no reflection) ─────────────

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


def crc8_compute(data: bytes) -> int:
    """CRC-8 with polynomial 0x07, initial value 0x00, no reflection."""
    crc = 0x00
    for b in data:
        crc = CRC8_TABLE[crc ^ b]
    return crc


# ── COBS encode / decode ──────────────────────────────────────────

def cobs_encode(src: bytes) -> bytes:
    """Consistent Overhead Byte Stuffing — encode."""
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


def cobs_decode(src: bytes) -> bytes | None:
    """COBS decode.  Returns None on invalid input."""
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


# ── Frame encode / decode ─────────────────────────────────────────

def frame_encode(cmd: int, data: bytes = b"", dir_: int = DIR_PI_TO_STM32) -> bytes:
    """Encode a frame: COBS(payload) | CRC8 | 0x00.

    Payload = CMD(1) + DIR(1) + LEN_H(1) + LEN_L(1) + DATA(N)
    """
    data_len = len(data)
    if data_len > MAX_DATA_LEN:
        raise ValueError(f"data length {data_len} exceeds MAX_DATA_LEN ({MAX_DATA_LEN})")

    payload = bytearray(4 + data_len)
    payload[0] = cmd
    payload[1] = dir_
    payload[2] = (data_len >> 8) & 0xFF
    payload[3] = data_len & 0xFF
    payload[4:] = data

    crc = crc8_compute(bytes(payload))
    cobs_payload = cobs_encode(bytes(payload))
    return cobs_payload + bytes([crc, FRAME_DELIMITER])


def frame_decode(raw: bytes) -> dict | None:
    """Decode a frame (raw bytes between delimiters, excluding trailing 0x00).

    Returns {"cmd": int, "dir": int, "data": bytes} or None.
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

    return {"cmd": cmd, "dir": dir_, "data": bytes(payload[4:])}


# ── Byte-level frame parser (state machine, matches frame_protocol.c) ──

class FrameParser:
    """Byte-at-a-time frame parser, identical logic to STM32 frame_parser_feed."""

    def __init__(self):
        self._buf = bytearray()
        self._error = False

    def reset(self):
        self._buf.clear()
        self._error = False

    def feed(self, byte: int) -> dict | None:
        """Feed one byte.  Returns decoded frame dict, or None if incomplete."""
        if byte == FRAME_DELIMITER:
            if len(self._buf) > 0:
                result = frame_decode(bytes(self._buf))
                self._buf.clear()
                if result is None:
                    self._error = True
                return result
            return None

        if len(self._buf) >= MAX_COBS_LEN:
            self._buf.clear()
            return None

        self._buf.append(byte)
        return None

    @property
    def had_error(self) -> bool:
        had = self._error
        self._error = False
        return had
