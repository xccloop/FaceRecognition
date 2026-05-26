#include "frame_protocol.h"

/* ════════════════════════════════════════════════════════════════
 *  CRC8 查表 (多项式 0x07, 初始值 0x00, 无反射)
 * ════════════════════════════════════════════════════════════════ */

static const uint8_t crc8_table[256] = {
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
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
};

static uint8_t crc8_compute(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    while (len--)
        crc = crc8_table[crc ^ *data++];
    return crc;
}

/* ════════════════════════════════════════════════════════════════
 *  COBS 编码 / 解码
 * ════════════════════════════════════════════════════════════════ */

static uint16_t cobs_encode(const uint8_t *src, uint16_t src_len, uint8_t *dst)
{
    const uint8_t *src_end = src + src_len;
    uint8_t *dst_start = dst;
    uint8_t *code_ptr;
    uint8_t code = 1;

    code_ptr = dst++;
    while (src < src_end) {
        if (*src == 0x00) {
            *code_ptr = code;
            code = 1;
            code_ptr = dst++;
            src++;
        } else {
            *dst++ = *src++;
            code++;
            if (code == 0xFF) {
                *code_ptr = code;
                code = 1;
                code_ptr = dst++;
            }
        }
    }
    *code_ptr = code;
    return (uint16_t)(dst - dst_start);
}

static int cobs_decode(const uint8_t *src, uint16_t src_len, uint8_t *dst, uint16_t dst_capacity)
{
    const uint8_t *src_end = src + src_len;
    uint8_t *dst_start = dst;

    while (src < src_end) {
        uint8_t code = *src++;
        if (code == 0) return -1; /* 无效的 COBS 数据 */

        uint8_t i;
        for (i = 1; i < code && src < src_end; i++) {
            if ((uint16_t)(dst - dst_start) >= dst_capacity) return -2;
            *dst++ = *src++;
        }
        if (src > src_end) return -3;

        if (code < 0xFF && src < src_end) {
            if ((uint16_t)(dst - dst_start) >= dst_capacity) return -2;
            *dst++ = 0x00;
        }
    }
    return (int)(dst - dst_start);
}

/* ════════════════════════════════════════════════════════════════
 *  帧编码
 * ════════════════════════════════════════════════════════════════ */

uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out_buf, uint16_t out_capacity)
{
    uint8_t payload[MAX_DATA_LEN + 4];
    uint8_t cobs_buf[MAX_COBS_LEN];
    uint16_t payload_len, cobs_len;
    uint8_t crc;

    if (data_len > MAX_DATA_LEN) return 0;

    payload[0] = cmd;
    payload[1] = dir;
    payload[2] = (uint8_t)((data_len >> 8) & 0xFF);
    payload[3] = (uint8_t)(data_len & 0xFF);

    uint16_t i;
    for (i = 0; i < data_len; i++)
        payload[4 + i] = data[i];
    payload_len = 4 + data_len;

    crc = crc8_compute(payload, payload_len);

    cobs_len = cobs_encode(payload, payload_len, cobs_buf);

    if (cobs_len + 1 + 1 > out_capacity) return 0;

    for (i = 0; i < cobs_len; i++)
        out_buf[i] = cobs_buf[i];
    out_buf[cobs_len] = crc;
    out_buf[cobs_len + 1] = FRAME_DELIMITER;

    return cobs_len + 2;
}

/* ════════════════════════════════════════════════════════════════
 *  逐字节帧解析器
 * ════════════════════════════════════════════════════════════════ */

static FrameState_t  g_state       = STATE_WAIT_DELIMITER;
static uint8_t       g_buf[MAX_COBS_LEN];
static uint16_t      g_buf_count   = 0;
static uint32_t      g_last_tick   = 0;
static int           g_error_flag  = 0;

void frame_parser_reset(void)
{
    g_state     = STATE_WAIT_DELIMITER;
    g_buf_count = 0;
    g_error_flag = 0;
}

void frame_parser_check_timeout(uint32_t current_tick)
{
    if (g_state == STATE_WAIT_DELIMITER && g_buf_count == 0) {
        g_last_tick = current_tick;
        return;
    }
    if ((current_tick - g_last_tick) >= FRAME_BYTE_TIMEOUT_MS) {
        g_state     = STATE_WAIT_DELIMITER;
        g_buf_count = 0;
    }
}

int frame_parser_feed(uint8_t ch, uint32_t current_tick, ParsedFrame_t *out_frame)
{
    g_last_tick = current_tick;

    switch (g_state) {

    case STATE_WAIT_DELIMITER:
        if (ch == FRAME_DELIMITER) {
            /* 帧间分隔符，忽略 */
            break;
        }
        g_buf[g_buf_count++] = ch;
        g_state = STATE_DATA;
        break;

    case STATE_DATA:
        if (g_buf_count >= MAX_COBS_LEN) {
            /* 缓冲区溢出 — 超长帧，丢弃 */
            g_state     = STATE_WAIT_DELIMITER;
            g_buf_count = 0;
            break;
        }
        if (ch == FRAME_DELIMITER) {
            g_state = STATE_GOT_DELIMITER;
            /* 直接跳到 GOT_DELIMITER 处理 */
            goto process_frame;
        }
        g_buf[g_buf_count++] = ch;
        break;

    case STATE_GOT_DELIMITER:
    process_frame:
    {
        uint8_t cobs_buf[MAX_COBS_LEN];
        uint16_t cobs_len;
        uint8_t received_crc;
        uint8_t payload[MAX_DATA_LEN + 4];
        int decoded_len;

        /* 至少需要 CRC8 + 1 字节 COBS overhead */
        if (g_buf_count < 2) {
            g_error_flag = 1;
            g_state     = STATE_WAIT_DELIMITER;
            g_buf_count = 0;
            return 0;
        }

        received_crc = g_buf[g_buf_count - 1];
        cobs_len     = g_buf_count - 1;

        uint16_t i;
        for (i = 0; i < cobs_len; i++)
            cobs_buf[i] = g_buf[i];

        decoded_len = cobs_decode(cobs_buf, cobs_len, payload, sizeof(payload));
        if (decoded_len < 0) {
            g_error_flag = 1;
            g_state     = STATE_WAIT_DELIMITER;
            g_buf_count = 0;
            return 0;
        }

        /* 长度超限检查 */
        if (decoded_len > (int)(MAX_DATA_LEN + 4) || decoded_len < 4) {
            g_error_flag = 1;
            g_state     = STATE_WAIT_DELIMITER;
            g_buf_count = 0;
            return 0;
        }

        /* CRC8 校验 */
        if (crc8_compute(payload, (uint16_t)decoded_len) != received_crc) {
            g_error_flag = 1;
            g_state     = STATE_WAIT_DELIMITER;
            g_buf_count = 0;
            return 0;
        }

        /* 填充输出帧 */
        if (out_frame) {
            out_frame->cmd = payload[0];
            out_frame->dir = payload[1];
            out_frame->len = ((uint16_t)payload[2] << 8) | payload[3];

            /* LEN 字段有效性检查 */
            if (out_frame->len > MAX_DATA_LEN) {
                g_error_flag = 1;
                g_state     = STATE_WAIT_DELIMITER;
                g_buf_count = 0;
                return 0;
            }
            if (out_frame->len != (uint16_t)(decoded_len - 4)) {
                g_error_flag = 1;
                g_state     = STATE_WAIT_DELIMITER;
                g_buf_count = 0;
                return 0;
            }
            for (i = 0; i < out_frame->len; i++)
                out_frame->data[i] = payload[4 + i];
        }

        g_state     = STATE_WAIT_DELIMITER;
        g_buf_count = 0;
        return 1;
    }
    } /* switch */

    return 0;
}

int frame_parser_had_error(void)
{
    int had = g_error_flag;
    g_error_flag = 0;
    return had;
}
