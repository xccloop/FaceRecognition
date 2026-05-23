#include "cmd_handler.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── 来自 main.c 的调试输出函数 ── */
extern void uart_send_text_line(const char *msg);
extern void uart_send_frame(uint8_t cmd, uint8_t dir,
                            const uint8_t *data, uint16_t data_len);

/* ── 来自 main.c 的心跳状态 ── */
extern TickType_t xLastFrameTicks;
extern uint8_t    serial_heartbeat_timeout_sent;
extern uint8_t    serial_heartbeat_restored;

/* ════════════════════════════════════════════════════════════════
 *  命令处理器
 * ════════════════════════════════════════════════════════════════ */

static void handle_identify(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[OK] IDENTIFY: face recognized!");
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, NULL, 0);
}

static void handle_unknown(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[WARN] UNKNOWN face detected!");
}

static void handle_noface(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[INFO] NOFACE - standby");
}

static void handle_multiface(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[WARN] MULTIFACE: multiple faces detected!");
}

static void handle_heartbeat(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, (const uint8_t *)"HB", 2);
    uart_send_text_line("[OK] HEARTBEAT received");
}

/* ════════════════════════════════════════════════════════════════
 *  帧分发（入口）
 * ════════════════════════════════════════════════════════════════ */

void dispatch_frame(const ParsedFrame_t *f)
{
    if (f->dir != DIR_PI_TO_STM32)
        return;

    xLastFrameTicks = xTaskGetTickCount();

    if (serial_heartbeat_timeout_sent && !serial_heartbeat_restored)
    {
        uart_send_text_line("[OK] HEARTBEAT RESTORED - Pi back online");
        serial_heartbeat_restored     = 1;
        serial_heartbeat_timeout_sent = 0;
    }

    switch (f->cmd)
    {
    case CMD_IDENTIFY:   handle_identify(f);   break;
    case CMD_UNKNOWN:    handle_unknown(f);    break;
    case CMD_NOFACE:     handle_noface(f);     break;
    case CMD_MULTIFACE:  handle_multiface(f);  break;
    case CMD_HEARTBEAT:  handle_heartbeat(f);  break;
    default: break;
    }
}
