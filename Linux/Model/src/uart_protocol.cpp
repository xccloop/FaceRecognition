#include "uart_protocol.h"
#include <cstring>

// ══════════════════════════════════════════════════════════
// CRC8 lookup table (poly 0x07, init 0x00, no reflection)
// ══════════════════════════════════════════════════════════

static const uint8_t CRC8_TABLE[256] = {
    0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,
    0x70,0x77,0x7E,0x79,0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,
    0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,
    0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,0xB4,0xB3,0xBA,0xBD,
    0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
    0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,
    0x27,0x20,0x29,0x2E,0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,
    0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,
    0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,0xAD,0xAA,0xA3,0xA4,
    0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
    0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,
    0x19,0x1E,0x17,0x10,0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,
    0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,
    0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,0x1A,0x1D,0x14,0x13,
    0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
    0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3,
};

uint8_t crc8_compute(const uint8_t* data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
        crc = CRC8_TABLE[crc ^ data[i]];
    return crc;
}

// ══════════════════════════════════════════════════════════
// COBS
// ══════════════════════════════════════════════════════════

std::vector<uint8_t> cobs_encode(const uint8_t* src, uint16_t len) {
    std::vector<uint8_t> result;
    if (len == 0) {
        result.push_back(0x01);
        return result;
    }
    result.reserve(len + len / 254 + 3);
    int code_ptr = 0;
    uint8_t code = 1;
    result.push_back(0);

    for (uint16_t i = 0; i < len; i++) {
        if (src[i] == 0x00) {
            result[code_ptr] = code;
            code = 1;
            code_ptr = (int)result.size();
            result.push_back(0);
        } else {
            result.push_back(src[i]);
            code++;
            if (code == 0xFF) {
                result[code_ptr] = code;
                code = 1;
                code_ptr = (int)result.size();
                result.push_back(0);
            }
        }
    }
    result[code_ptr] = code;
    return result;
}

std::vector<uint8_t> cobs_decode(const uint8_t* src, uint16_t len) {
    std::vector<uint8_t> result;
    result.reserve(len);
    uint16_t i = 0;
    while (i < len) {
        uint8_t code = src[i++];
        if (code == 0) return {};  // corrupt
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len) return {};
            result.push_back(src[i++]);
        }
        if (code < 0xFF && i < len)
            result.push_back(0x00);
    }
    return result;
}

// ══════════════════════════════════════════════════════════
// Frame encode / decode
// ══════════════════════════════════════════════════════════

std::vector<uint8_t> frame_encode(uint8_t cmd, uint8_t dir,
                                  const uint8_t* data, uint16_t data_len) {
    if (data_len > MAX_DATA_LEN) return {};

    std::vector<uint8_t> payload(4 + data_len);
    payload[0] = cmd;
    payload[1] = dir;
    payload[2] = (data_len >> 8) & 0xFF;
    payload[3] = data_len & 0xFF;
    if (data_len)
        memcpy(payload.data() + 4, data, data_len);

    uint8_t crc = crc8_compute(payload.data(), (uint16_t)payload.size());
    auto cobs = cobs_encode(payload.data(), (uint16_t)payload.size());

    std::vector<uint8_t> frame;
    frame.reserve(cobs.size() + 2);
    frame.insert(frame.end(), cobs.begin(), cobs.end());
    frame.push_back(crc);
    frame.push_back(FRAME_DELIMITER);
    return frame;
}

static bool frame_decode_raw(const uint8_t* raw, uint16_t len, ParsedFrame& out) {
    if (len < 2) return false;

    uint8_t received_crc = raw[len - 1];
    auto payload = cobs_decode(raw, len - 1);
    if (payload.empty() || payload.size() < 4 || payload.size() > MAX_DATA_LEN + 4)
        return false;

    uint8_t expected_crc = crc8_compute(payload.data(), (uint16_t)payload.size());
    if (expected_crc != received_crc) return false;

    out.cmd = payload[0];
    out.dir = payload[1];
    uint16_t dlen = ((uint16_t)payload[2] << 8) | payload[3];
    if (dlen != payload.size() - 4) return false;

    out.len = dlen;
    if (dlen)
        memcpy(out.data, payload.data() + 4, dlen);
    return true;
}

// ══════════════════════════════════════════════════════════
// FrameParser
// ══════════════════════════════════════════════════════════

void FrameParser::reset() {
    state_ = WAIT;
    pos_ = 0;
}

bool FrameParser::feed(uint8_t byte, ParsedFrame& out) {
    if (state_ == WAIT) {
        if (byte == FRAME_DELIMITER) return false;
        buf_[pos_++] = byte;
        state_ = DATA;
        return false;
    }

    // STATE_DATA
    if (pos_ >= MAX_COBS_LEN) {
        reset();
        return false;
    }

    if (byte == FRAME_DELIMITER) {
        bool ok = frame_decode_raw(buf_, pos_, out);
        reset();
        return ok;
    }

    buf_[pos_++] = byte;
    return false;
}

// ══════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════

const char* cmd_name(uint8_t cmd) {
    switch (cmd) {
    case CMD_IDENTIFY:  return "IDENTIFY";
    case CMD_UNKNOWN:   return "UNKNOWN";
    case CMD_NOFACE:    return "NOFACE";
    case CMD_MULTIFACE: return "MULTIFACE";
    case CMD_HEARTBEAT: return "HEARTBEAT";
    case CMD_ACK:       return "ACK";
    default:            return "?";
    }
}
