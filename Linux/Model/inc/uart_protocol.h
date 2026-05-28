#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ── Frame constants (match STM32 frame_protocol.h) ──
#define FRAME_DELIMITER  0x00
#define MAX_DATA_LEN     64
#define MAX_FRAME_LEN    (MAX_DATA_LEN + 4)
// COBS worst case: N + ceil(N/254) + CRC8 + delimiter
#define MAX_COBS_LEN     (MAX_FRAME_LEN + MAX_FRAME_LEN / 254 + 3)

// ── Direction ──
#define DIR_PI_TO_STM32   0x01
#define DIR_STM32_TO_PI   0x02

// ── Commands ──
#define CMD_IDENTIFY   0x10
#define CMD_UNKNOWN    0x11
#define CMD_NOFACE     0x12
#define CMD_MULTIFACE  0x13
#define CMD_HEARTBEAT  0x1F
#define CMD_ACK        0x20

// ══════════════════════════════════════════════════════════
// CRC8 — polynomial 0x07, init 0x00, no reflection
// ══════════════════════════════════════════════════════════
uint8_t crc8_compute(const uint8_t* data, uint16_t len);

// ══════════════════════════════════════════════════════════
// COBS encode / decode
// ══════════════════════════════════════════════════════════
std::vector<uint8_t> cobs_encode(const uint8_t* src, uint16_t len);
std::vector<uint8_t> cobs_decode(const uint8_t* src, uint16_t len);  // empty = fail

// ══════════════════════════════════════════════════════════
// Frame encode / decode
// ══════════════════════════════════════════════════════════
// Encode: cmd + data → COBS + CRC8 + 0x00
std::vector<uint8_t> frame_encode(uint8_t cmd, uint8_t dir,
                                  const uint8_t* data, uint16_t data_len);

// Decoded frame struct
struct ParsedFrame {
    uint8_t cmd;
    uint8_t dir;
    uint16_t len;
    uint8_t  data[MAX_DATA_LEN];
};

// ══════════════════════════════════════════════════════════
// Frame parser — byte-by-byte state machine (matches STM32)
// ══════════════════════════════════════════════════════════
class FrameParser {
public:
    void reset();

    // Feed one byte. Returns true when a complete, valid frame is decoded.
    bool feed(uint8_t byte, ParsedFrame& out);

private:
    enum State { WAIT, DATA };
    State state_ = WAIT;
    uint8_t buf_[MAX_COBS_LEN];
    int     pos_ = 0;
};

// ══════════════════════════════════════════════════════════
// Command name helper
// ══════════════════════════════════════════════════════════
const char* cmd_name(uint8_t cmd);
