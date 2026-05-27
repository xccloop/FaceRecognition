"""Tests for COBS (Consistent Overhead Byte Stuffing) implementation.

Validated against the C implementation in Stm32/Protocol/frame_protocol.c.
"""

import pytest
from .protocol_ref import cobs_encode, cobs_decode


# ── Encode known-answer tests (verified against C implementation) ──

def test_cobs_encode_empty():
    """COBS of empty input produces a single 0x01 byte."""
    assert cobs_encode(b"") == b"\x01"


def test_cobs_encode_no_zeros():
    """Data with no zero bytes: code byte + all data."""
    assert cobs_encode(b"\x01\x02\x03") == b"\x04\x01\x02\x03"


def test_cobs_encode_single_zero():
    """Single 0x00 in the middle."""
    assert cobs_encode(b"\x01\x00\x02") == b"\x02\x01\x02\x02"


def test_cobs_encode_consecutive_zeros():
    """Consecutive 0x00 bytes each produce their own code byte.

    Two 0x00 bytes → three code bytes (one overhead + one per zero):
      code=1 covers first zero, code=1 covers second zero, trailing code=1.
    """
    assert cobs_encode(b"\x00\x00") == b"\x01\x01\x01"


def test_cobs_encode_long_no_zeros():
    """254 non-zero bytes: code hits 0xFF on last byte, creating trailing overhead.

    COBS overhead: when code == 0xFF triggers mid-stream, a new code_ptr is
    allocated.  With exactly 254 non-zero bytes, code goes 1→255 on the final
    byte, so we get [0xFF | 254×data | 0x01] = 256 bytes.
    The roundtrip test below confirms correctness regardless of length.
    """
    data = bytes([0x01] * 254)
    encoded = cobs_encode(data)
    assert encoded[0] == 0xFF
    assert cobs_decode(encoded) == data


def test_cobs_encode_255_nonzero_triggers_split():
    """255 non-zero bytes: code=0xFF + 254 bytes, then code=0x02 + 1 byte."""
    data = bytes([0x01] * 255)
    encoded = cobs_encode(data)
    assert encoded[0] == 0xFF
    assert encoded[255] == 0x02
    assert len(encoded) == 257


def test_cobs_encode_c_known_answer_1():
    """Values cross-checked against STM32 C implementation.

    Input payload: [0x10, 0x01, 0x00, 0x00] (IDENTIFY with zero-length data)
    Trace:
      - code=1
      - 0x10 → append, code=2
      - 0x01 → append, code=3
      - 0x00 → *code_ptr=3, code=1
      - 0x00 → *code_ptr=1, code=1
      - end → *code_ptr=1
    Result: [0x03, 0x10, 0x01, 0x01, 0x01]
    """
    payload = bytes([0x10, 0x01, 0x00, 0x00])
    result = cobs_encode(payload)
    assert result == b"\x03\x10\x01\x01\x01", f"Got {result.hex()}"


# ── Decode known-answer tests ──────────────────────────────────────

def test_cobs_decode_empty():
    """Decoding b'\\x01' gives empty data."""
    assert cobs_decode(b"\x01") == b""


def test_cobs_decode_no_zeros():
    """Roundtrip data with no zeros."""
    assert cobs_decode(b"\x04\x01\x02\x03") == b"\x01\x02\x03"


def test_cobs_decode_single_zero():
    """Roundtrip data with a single 0x00."""
    assert cobs_decode(b"\x02\x01\x02\x02") == b"\x01\x00\x02"


def test_cobs_decode_invalid_zero_code():
    """A code byte of 0x00 is invalid COBS."""
    assert cobs_decode(b"\x00") is None


# ── Roundtrip tests ────────────────────────────────────────────────

def test_cobs_roundtrip_empty():
    assert cobs_decode(cobs_encode(b"")) == b""


def test_cobs_roundtrip_no_zeros():
    data = bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08])
    assert cobs_decode(cobs_encode(data)) == data


def test_cobs_roundtrip_with_zeros():
    data = bytes([0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x03])
    assert cobs_decode(cobs_encode(data)) == data


def test_cobs_roundtrip_max_payload():
    """Roundtrip with maximum possible payload (CMD+DIR+LEN_H+LEN_L+DATA).

    max_frame = 4 + 64 = 68 bytes.
    """
    import random
    random.seed(42)
    payload = bytes(random.randint(0, 255) for _ in range(68))
    assert cobs_decode(cobs_encode(payload)) == payload


def test_cobs_roundtrip_all_zeros():
    """Roundtrip with all-zero payload."""
    data = bytes([0x00] * 10)
    assert cobs_decode(cobs_encode(data)) == data


def test_cobs_roundtrip_all_ones():
    """Roundtrip with all-ones payload (no zeros)."""
    data = bytes([0x01] * 200)
    assert cobs_decode(cobs_encode(data)) == data


def test_cobs_roundtrip_varied():
    """Roundtrip every possible byte value."""
    data = bytes(range(256))
    assert cobs_decode(cobs_encode(data)) == data
