"""Tests for CRC8 implementation.

The CRC8 table must match bit-for-bit between:
  - Stm32/Protocol/frame_protocol.c  (crc8_table[256])
  - Linux/Raspberry Pi/src/uart.py   (CRC8_TABLE)
  - tests/protocol/protocol_ref.py   (CRC8_TABLE)
"""

import pytest
from .protocol_ref import crc8_compute, CRC8_TABLE


def test_crc8_table_length():
    """CRC8 table must have exactly 256 entries."""
    assert len(CRC8_TABLE) == 256


def test_crc8_empty_input():
    """CRC8 of empty input is 0x00 (initial value)."""
    assert crc8_compute(b"") == 0x00


def test_crc8_single_byte():
    """CRC8 of a single 0x10 byte (IDENTIFY command)."""
    assert crc8_compute(bytes([0x10])) == CRC8_TABLE[0x10]


def test_crc8_known_answer_1():
    """CRC8("123456789") is a well-known CRC-8 test vector.

    CRC-8/DVB-S2 (poly=0xD5) gives 0xBC, but our poly=0x07 gives 0xF4.
    This is a deliberate design choice: poly 0x07 is simpler and runs faster
    on Cortex-M3.
    """
    assert crc8_compute(b"123456789") == 0xF4


def test_crc8_known_answer_2():
    """CRC8 for IDENTIFY frame payload: [0x10, 0x01, 0x00, 0x00]."""
    payload = bytes([0x10, 0x01, 0x00, 0x00])
    assert crc8_compute(payload) == 0x0C


def test_crc8_known_answer_3():
    """CRC8 for HEARTBEAT frame payload: [0x1F, 0x01, 0x00, 0x00]."""
    payload = bytes([0x1F, 0x01, 0x00, 0x00])
    assert crc8_compute(payload) == 0xDE


def test_crc8_reproducible():
    """Same input must always produce the same CRC."""
    for _ in range(10):
        assert crc8_compute(bytes([0xAA, 0xBB, 0xCC])) == 0x7D


def test_crc8_vs_reference_implementation():
    """Verify the table-based algorithm matches the bit-by-bit definition.

    CRC-8 with poly 0x07: for each byte, XOR into CRC, then 8 shifts:
      if MSB=1: crc = (crc << 1) ^ poly
      else:     crc = crc << 1
    """
    def crc8_bitwise(data: bytes) -> int:
        crc = 0x00
        for byte in data:
            crc ^= byte
            for _ in range(8):
                if crc & 0x80:
                    crc = ((crc << 1) ^ 0x07) & 0xFF
                else:
                    crc = (crc << 1) & 0xFF
        return crc

    test_vectors = [
        b"",
        bytes([0x00]),
        bytes([0xFF]),
        bytes([0x10, 0x01, 0x00, 0x00]),
        bytes(range(64)),
        b"hello world",
    ]
    for tv in test_vectors:
        assert crc8_compute(tv) == crc8_bitwise(tv), f"Failed for {tv!r}"
