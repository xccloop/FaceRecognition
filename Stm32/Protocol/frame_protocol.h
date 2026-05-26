/* ================================================================
 *  frame_protocol.h — FaceRecognition 帧协议定义 (v2: COBS+CRC8)
 *
 *  帧格式:
 *  ┌──────────────────────────┬──────┬──────┐
 *  │ COBS(CMD+DIR+LEN_H+LEN_L+DATA) │ CRC8 │ 0x00 │
 *  └──────────────────────────┴──────┴──────┘
 *                              校验    帧尾
 *
 *  方向 DIR: 0x01 = Pi/PC → STM32,  0x02 = STM32 → Pi/PC
 *
 *  CRC8 参数: 多项式 0x07 / 初始值 0x00 / 无反射
 *  CRC8 计算对象: 原始 payload (CMD+DIR+LEN_H+LEN_L+DATA)
 *
 *  ★ PC 串口助手测试需使用 HEX 模式发送
 *
 * ================================================================ */

#ifndef FRAME_PROTOCOL_H
#define FRAME_PROTOCOL_H

#include <stdint.h>

/* ── 帧格式常量 ── */
#define FRAME_DELIMITER  0x00
#define CRC8_POLY        0x07
#define MAX_DATA_LEN     64
#define MAX_FRAME_LEN    ((MAX_DATA_LEN) + 4)        /* CMD+DIR+LEN_H+LEN_L+DATA */
#define MAX_COBS_LEN     ((MAX_FRAME_LEN) + (MAX_FRAME_LEN)/254 + 3) /* COBS 编码后 + CRC8 + delimiter */
#define FRAME_BYTE_TIMEOUT_MS  5

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

/* ── 帧解析状态 ── */
typedef enum {
    STATE_WAIT_DELIMITER,      /* 等待/跳过 0x00，收到非 0x00 进入 DATA */
    STATE_DATA,                /* 收集字节直到 0x00 */
    STATE_GOT_DELIMITER        /* 收到 0x00，解码+校验 */
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

/* 编码一帧到 out_buf (COBS + CRC8 + 0x00)
 * 返回总字节数；0 表示 out_capacity 不足
 */
uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out_buf, uint16_t out_capacity);

/* 逐字节喂入解析器。
 * current_tick: 当前系统 tick (ms)，用于字节间超时检测。
 * 返回 1 表示完成一帧（out_frame 有效），0 表示还需更多字节。
 */
int frame_parser_feed(uint8_t byte, uint32_t current_tick, ParsedFrame_t *out_frame);

/* 超时检查：若距上次 feed 超过 FRAME_BYTE_TIMEOUT_MS，重置解析器。
 * 应由主循环或定时器周期性调用（建议每 2ms）。
 * 参数 current_tick: 当前系统 tick 值（ms）。
 */
void frame_parser_check_timeout(uint32_t current_tick);

/* 重置解析器到初始状态 */
void frame_parser_reset(void);

/* 查询上一次帧结束(收到 0x00)时是否发生了解码/校验错误。
   调用后自动清零。 */
int frame_parser_had_error(void);

#endif /* FRAME_PROTOCOL_H */
