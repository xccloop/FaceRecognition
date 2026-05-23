#include "frame_protocol.h"

/* ════════════════════════════════════════════════════════════════
 *  帧编码（转义 + 组帧）
 * ════════════════════════════════════════════════════════════════ */

uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out_buf, uint16_t out_capacity)
{
    uint16_t pos = 0;
    uint8_t  xor_val;
    uint16_t i;

    if (out_capacity < (7 + 2 * data_len))
        return 0;

    out_buf[pos++] = FRAME_HEADER;
    out_buf[pos++] = cmd;
    out_buf[pos++] = dir;
    out_buf[pos++] = (uint8_t)((data_len >> 8) & 0xFF);
    out_buf[pos++] = (uint8_t)(data_len & 0xFF);

    for (i = 0; i < data_len; i++)
    {
        uint8_t b = data[i];
        if (b == FRAME_HEADER)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_A; }
        else if (b == FRAME_TAIL)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_B; }
        else if (b == ESCAPE_BYTE)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_SELF; }
        else
            out_buf[pos++] = b;
    }

    xor_val = cmd ^ dir
            ^ (uint8_t)((data_len >> 8) & 0xFF)
            ^ (uint8_t)(data_len & 0xFF);
    for (i = 0; i < data_len; i++)
        xor_val ^= data[i];
    out_buf[pos++] = xor_val;
    out_buf[pos++] = FRAME_TAIL;

    return pos;
}

/* ════════════════════════════════════════════════════════════════
 *  反转义辅助函数
 * ════════════════════════════════════════════════════════════════ */

static uint8_t unescape_byte(uint8_t b, uint8_t *out, uint8_t *esc_state)
{
    if (*esc_state)
    {
        *esc_state = 0;
        if (b == ESCAPE_XOR_A)         *out = FRAME_HEADER;
        else if (b == ESCAPE_XOR_B)    *out = FRAME_TAIL;
        else if (b == ESCAPE_XOR_SELF) *out = ESCAPE_BYTE;
        else return 0;
        return 1;
    }
    if (b == ESCAPE_BYTE) { *esc_state = 1; return 0; }
    *out = b;
    return 1;
}

/* ════════════════════════════════════════════════════════════════
 *  逐字节帧解析器（全局唯一状态机，适合单 UART 场景）
 * ════════════════════════════════════════════════════════════════ */

static FrameState_t  g_state     = STATE_SYNC;
static uint8_t       g_esc_state = 0;
static uint16_t      g_data_idx  = 0;
static uint8_t       g_xor_calc  = 0;
static ParsedFrame_t g_frame;

void frame_parser_reset(void)
{
    g_state     = STATE_SYNC;
    g_esc_state = 0;
    g_data_idx  = 0;
    g_xor_calc  = 0;
}

/* 返回 1: 完成一帧，out_frame 有效
 * 返回 0: 还需更多字节 */
int frame_parser_feed(uint8_t ch, ParsedFrame_t *out_frame)
{
    switch (g_state)
    {
    case STATE_SYNC:
        if (ch == FRAME_HEADER)
            g_state = STATE_CMD;
        break;

    case STATE_CMD:
        g_frame.cmd  = ch;
        g_xor_calc   = ch;
        g_state      = STATE_DIR;
        break;

    case STATE_DIR:
        g_frame.dir  = ch;
        g_xor_calc  ^= ch;
        g_state      = STATE_LEN_H;
        break;

    case STATE_LEN_H:
        g_frame.len  = ((uint16_t)ch) << 8;
        g_xor_calc  ^= ch;
        g_state      = STATE_LEN_L;
        break;

    case STATE_LEN_L:
        g_frame.len |= ch;
        g_xor_calc  ^= ch;
        if (g_frame.len > MAX_DATA_LEN)
        {
            /* 帧太长，丢弃 */
            g_state     = STATE_SYNC;
            g_esc_state = 0;
            break;
        }
        g_data_idx  = 0;
        g_esc_state = 0;
        g_state     = (g_frame.len > 0) ? STATE_DATA : STATE_XOR;
        break;

    case STATE_DATA:
    {
        uint8_t raw_byte;
        if (unescape_byte(ch, &raw_byte, &g_esc_state))
        {
            g_frame.data[g_data_idx++] = raw_byte;
            g_xor_calc                ^= raw_byte;
            if (g_data_idx >= g_frame.len)
                g_state = STATE_XOR;
        }
        break;
    }

    case STATE_XOR:
        if (ch != g_xor_calc)
        {
            /* XOR 校验失败 */
            g_state     = STATE_SYNC;
            g_esc_state = 0;
            break;
        }
        g_state = STATE_TAIL;
        break;

    case STATE_TAIL:
        if (ch == FRAME_TAIL)
        {
            /* 完整帧成功，复制输出并重置 */
            if (out_frame)
                *out_frame = g_frame;
            g_state     = STATE_SYNC;
            g_esc_state = 0;
            return 1;
        }
        /* 帧尾错误，回到同步 */
        g_state     = STATE_SYNC;
        g_esc_state = 0;
        break;

    } /* switch(g_state) */

    return 0;
}
