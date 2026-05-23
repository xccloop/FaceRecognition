/* ================================================================
 *  frame_protocol.h — FaceRecognition 帧协议定义
 *
 *  帧格式 Frame Format:
 *  ┌──────┬─────┬─────┬───────┬───────┬──────────────┬─────┬──────┐
 *  │ 0xAA │ CMD │ DIR │ LEN_H │ LEN_L │ DATA (转义)  │ XOR │ 0x55 │
 *  └──────┴─────┴─────┴───────┴───────┴──────────────┴─────┴──────┘
 *   帧头                    大端长度              校验  帧尾
 *
 *  方向 DIR: 0x01 = Pi/PC → STM32,  0x02 = STM32 → Pi/PC
 *
 *  转义 Escape (仅 DATA 段):
 *    0xAA → 0xBB 0x55    0x55 → 0xBB 0xAA    0xBB → 0xBB 0x44
 *
 *  XOR = CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
 *  (帧头 0xAA 和帧尾 0x55 不参与 XOR)
 *
 *  ── PC 串口助手测试命令 (HEX 模式发送) ─────────────────────
 *
 *  心跳:       AA 1F 01 00 00 1E 55
 *  识别成功:   AA 10 01 00 00 11 55
 *  未注册人脸: AA 11 01 00 00 10 55
 *  无人脸:     AA 12 01 00 00 13 55
 *  多人脸:     AA 13 01 00 00 12 55
 *
 *  ★ 务必使用 HEX(十六进制)发送模式，不能用 ASCII 文本模式
 *
 * ================================================================ */

#ifndef FRAME_PROTOCOL_H
#define FRAME_PROTOCOL_H

#include <stdint.h>

/* ── 帧格式常量 ── */
#define FRAME_HEADER   0xAA
#define FRAME_TAIL     0x55
#define ESCAPE_BYTE    0xBB
#define ESCAPE_XOR_A   0x55   /* 0xBB 0x55 -> 0xAA */
#define ESCAPE_XOR_B   0xAA   /* 0xBB 0xAA -> 0x55 */
#define ESCAPE_XOR_SELF 0x44  /* 0xBB 0x44 -> 0xBB */

/* ── 方向 ── */
#define DIR_PI_TO_STM32   0x01
#define DIR_STM32_TO_PI   0x02

/* ── 命令字 ── */
#define CMD_IDENTIFY   0x10   /* 识别成功 */
#define CMD_UNKNOWN    0x11   /* 检测到人脸但未识别 */
#define CMD_NOFACE     0x12   /* 无人脸 */
#define CMD_MULTIFACE  0x13   /* 多人脸 */
#define CMD_HEARTBEAT  0x1F   /* 心跳 */
#define CMD_ACK        0x20   /* STM32->Pi: 确认 */

#define MAX_DATA_LEN   64

/* ── 帧解析状态 ── */
typedef enum {
    STATE_SYNC,           /* 搜索帧头 0xAA */
    STATE_CMD,
    STATE_DIR,
    STATE_LEN_H,
    STATE_LEN_L,
    STATE_DATA,           /* 读取 LEN 字节，含转义处理 */
    STATE_XOR,
    STATE_TAIL
} FrameState_t;

/* ── 解析出的帧结构 ── */
typedef struct {
    uint8_t  cmd;
    uint8_t  dir;
    uint16_t len;
    uint8_t  data[MAX_DATA_LEN];
} ParsedFrame_t;

/* ════════════════════════════════════════════════════════════════
 *  API
 * ════════════════════════════════════════════════════════════════ */

/* 编码一帧到 out_buf，含转义和校验。
 * 返回总字节数；0 表示 out_capacity 不足。
 */
uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out_buf, uint16_t out_capacity);

/* 逐字节喂入解析器。
 * 返回 1 表示完成一帧（out_frame 有效），0 表示还需更多字节。
 * 解析器内部状态为 static，全局唯一——适合单 UART 场景。
 */
int frame_parser_feed(uint8_t byte, ParsedFrame_t *out_frame);

/* 重置解析器到初始状态（STATE_SYNC） */
void frame_parser_reset(void);

#endif /* FRAME_PROTOCOL_H */
