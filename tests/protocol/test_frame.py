"""Tests for full frame encode/decode (COBS + CRC8 + 0x00 delimiter).

Validates end-to-end frame construction and parsing, matching the wire
format used between Raspberry Pi and STM32.
"""

import pytest
from .protocol_ref import (
    frame_encode, frame_decode, FrameParser, MAX_DATA_LEN,
    DIR_PI_TO_STM32, DIR_STM32_TO_PI,
    CMD_IDENTIFY, CMD_UNKNOWN, CMD_NOFACE, CMD_MULTIFACE, CMD_HEARTBEAT, CMD_ACK,
    FRAME_DELIMITER,
)


# ── Frame encode tests ────────────────────────────────────────────

def test_frame_encode_heartbeat():
    """HEARTBEAT frame: CMD=0x1F, DIR=0x01, no data."""
    frame = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)
    assert len(frame) >= 3
    assert frame[-1] == FRAME_DELIMITER


def test_frame_encode_identify():
    """IDENTIFY frame with a name."""
    name = "张三".encode("utf-8")
    frame = frame_encode(CMD_IDENTIFY, name, DIR_PI_TO_STM32)
    assert frame[-1] == FRAME_DELIMITER


def test_frame_encode_empty_data():
    """Frame with zero-length data payload."""
    frame = frame_encode(CMD_NOFACE, b"", DIR_PI_TO_STM32)
    assert frame[-1] == FRAME_DELIMITER
    assert len(frame) >= 3


def test_frame_encode_max_data():
    """Frame with maximum allowed data length (64 bytes)."""
    data = bytes([0x42] * MAX_DATA_LEN)
    frame = frame_encode(CMD_IDENTIFY, data, DIR_PI_TO_STM32)
    assert frame[-1] == FRAME_DELIMITER


def test_frame_encode_too_long_raises():
    """Data longer than MAX_DATA_LEN must raise ValueError."""
    with pytest.raises(ValueError):
        frame_encode(CMD_IDENTIFY, bytes([0x42] * (MAX_DATA_LEN + 1)))


def test_frame_encode_data_includes_zero():
    """COBS must handle 0x00 bytes in payload data correctly."""
    data = bytes([0x00, 0x01, 0x00])
    frame = frame_encode(CMD_IDENTIFY, data, DIR_PI_TO_STM32)
    decoded = frame_decode(frame[:-1])  # strip delimiter
    assert decoded is not None
    assert decoded["data"] == data


# ── Frame decode tests ────────────────────────────────────────────

def test_frame_decode_roundtrip_identify():
    """Encode then decode an IDENTIFY frame."""
    name = "李四".encode("utf-8")
    frame = frame_encode(CMD_IDENTIFY, name, DIR_PI_TO_STM32)
    decoded = frame_decode(frame[:-1])  # strip delimiter
    assert decoded is not None
    assert decoded["cmd"] == CMD_IDENTIFY
    assert decoded["dir"] == DIR_PI_TO_STM32
    assert decoded["data"] == name


def test_frame_decode_roundtrip_ack():
    """Encode then decode an ACK frame."""
    frame = frame_encode(CMD_ACK, b"HB", DIR_STM32_TO_PI)
    decoded = frame_decode(frame[:-1])
    assert decoded is not None
    assert decoded["cmd"] == CMD_ACK
    assert decoded["dir"] == DIR_STM32_TO_PI
    assert decoded["data"] == b"HB"


def test_frame_decode_roundtrip_noface():
    """Encode then decode a NOFACE frame (empty data)."""
    frame = frame_encode(CMD_NOFACE, b"", DIR_PI_TO_STM32)
    decoded = frame_decode(frame[:-1])
    assert decoded is not None
    assert decoded["cmd"] == CMD_NOFACE
    assert decoded["dir"] == DIR_PI_TO_STM32
    assert decoded["data"] == b""


def test_frame_decode_roundtrip_max_data():
    """Roundtrip with 64-byte payload."""
    data = bytes(range(64))
    frame = frame_encode(CMD_IDENTIFY, data, DIR_PI_TO_STM32)
    decoded = frame_decode(frame[:-1])
    assert decoded is not None
    assert decoded["data"] == data


# ── CRC corruption detection ──────────────────────────────────────

def test_frame_crc_error_detected():
    """Flipping a bit in the CRC byte must cause decode to fail."""
    frame = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)
    raw = bytearray(frame[:-1])  # strip delimiter
    raw[-2] ^= 0x01  # flip last bit of CRC
    assert frame_decode(bytes(raw)) is None


def test_frame_crc_error_on_data():
    """Flipping a data byte must cause CRC mismatch."""
    frame = frame_encode(CMD_IDENTIFY, b"test", DIR_PI_TO_STM32)
    raw = bytearray(frame[:-1])
    raw[2] ^= 0x80  # flip a bit in the COBS payload
    assert frame_decode(bytes(raw)) is None


def test_frame_truncated_rejected():
    """Truncated frame must be rejected."""
    frame = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)
    truncated = frame[1:-1]  # remove first byte and delimiter
    assert frame_decode(truncated) is None


# ── Byte-level parser tests ───────────────────────────────────────

def test_parser_single_frame():
    """Feed a complete frame to the parser byte by byte."""
    parser = FrameParser()
    frame = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)

    result = None
    for b in frame:
        r = parser.feed(b)
        if r is not None:
            result = r

    assert result is not None
    assert result["cmd"] == CMD_HEARTBEAT
    assert not parser.had_error


def test_parser_two_frames():
    """Feed two consecutive frames — the parser must reset between them."""
    parser = FrameParser()
    f1 = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)
    f2 = frame_encode(CMD_IDENTIFY, "王五".encode("utf-8"), DIR_PI_TO_STM32)

    results = []
    for b in f1 + f2:
        r = parser.feed(b)
        if r is not None:
            results.append(r)

    assert len(results) == 2
    assert results[0]["cmd"] == CMD_HEARTBEAT
    assert results[1]["cmd"] == CMD_IDENTIFY
    assert results[1]["data"] == "王五".encode("utf-8")


def test_parser_garbage_byte():
    """A garbage byte between frames must not hang the parser."""
    parser = FrameParser()
    frame = frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32)

    # Delimiter, then garbage, then valid frame
    stream = bytes([FRAME_DELIMITER, FRAME_DELIMITER, FRAME_DELIMITER]) + frame
    result = None
    for b in stream:
        r = parser.feed(b)
        if r is not None:
            result = r

    assert result is not None
    assert result["cmd"] == CMD_HEARTBEAT


def test_parser_corrupted_frame():
    """A frame with flipped CRC should set error flag and not return a frame."""
    parser = FrameParser()
    frame = bytearray(frame_encode(CMD_HEARTBEAT, b"", DIR_PI_TO_STM32))
    frame[-2] ^= 0xFF  # corrupt CRC

    result = None
    for b in frame:
        r = parser.feed(b)
        if r is not None:
            result = r

    assert result is None  # corrupted frame should not be returned
    assert parser.had_error


# ── Protocol ID uniqueness ────────────────────────────────────────

def test_command_ids_unique():
    """All command IDs must be distinct."""
    cmds = [CMD_IDENTIFY, CMD_UNKNOWN, CMD_NOFACE, CMD_MULTIFACE, CMD_HEARTBEAT, CMD_ACK]
    assert len(cmds) == len(set(cmds))


def test_direction_ids_unique():
    """Direction IDs must be distinct."""
    assert DIR_PI_TO_STM32 != DIR_STM32_TO_PI
