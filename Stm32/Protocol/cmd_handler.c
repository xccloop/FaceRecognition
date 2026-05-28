#include "cmd_handler.h"
#include "../BSP/LCD/display.h"
#include "../BSP/LED/led.h"
#include "FreeRTOS.h"
#include "task.h"

/* ── 来自 main.c ── */
extern void uart_send_frame(uint8_t cmd, uint8_t dir,
                            const uint8_t *data, uint16_t data_len);
extern TickType_t xLastFrameTicks;
extern uint8_t    serial_heartbeat_timeout_sent;
extern uint8_t    serial_heartbeat_restored;
extern uint8_t    serial_has_received_frame;

/* ════════════════════════════════════════════════════════════════
 *  命令处理器 (串口静默, 只回复 ACK 帧, 不打印文本)
 * ════════════════════════════════════════════════════════════════ */

static void handle_identify(const ParsedFrame_t *f)
{
    char name[32];
    uint16_t name_len;

    name_len = f->len;
    if (name_len > sizeof(name) - 1) name_len = sizeof(name) - 1;
    if (name_len > 0) {
        uint16_t i;
        for (i = 0; i < name_len; i++) name[i] = (char)f->data[i];
        name[name_len] = '\0';
    } else {
        name[0] = '?'; name[1] = '\0';
    }

    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, NULL, 0);
    display_show_success(name);
    led_success();
}

static void handle_unknown(const ParsedFrame_t *f)
{
    (void)f;
    display_show_failure(REASON_UNREGISTERED);
    led_failure();
}

static void handle_noface(const ParsedFrame_t *f)
{
    (void)f;
    display_show_standby();
}

static void handle_multiface(const ParsedFrame_t *f)
{
    (void)f;
    display_show_failure(REASON_MULTIFACE);
    led_failure();
}

static void handle_heartbeat(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, (const uint8_t *)"HB", 2);
}

/* ════════════════════════════════════════════════════════════════
 *  帧分发
 * ════════════════════════════════════════════════════════════════ */

void dispatch_frame(const ParsedFrame_t *f)
{
    if (f->dir != DIR_PI_TO_STM32)
        return;

    serial_has_received_frame = 1;
    xLastFrameTicks = xTaskGetTickCount();

    if (serial_heartbeat_timeout_sent && !serial_heartbeat_restored) {
        serial_heartbeat_restored     = 1;
        serial_heartbeat_timeout_sent = 0;
        display_show_standby();
        led_timeout_stop();
    }

    switch (f->cmd) {
    case CMD_IDENTIFY:   handle_identify(f);   break;
    case CMD_UNKNOWN:    handle_unknown(f);    break;
    case CMD_NOFACE:     handle_noface(f);     break;
    case CMD_MULTIFACE:  handle_multiface(f);  break;
    case CMD_HEARTBEAT:  handle_heartbeat(f);  break;
    default: break;
    }
}
