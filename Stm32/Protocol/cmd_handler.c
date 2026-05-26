#include "cmd_handler.h"
#include "stm32f10x.h"
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

/* ── LED 引脚: PB5=错误, PE5=成功 ── */
#define LED_ERR_PORT   GPIOB
#define LED_ERR_PIN    GPIO_Pin_5
#define LED_OK_PORT    GPIOE
#define LED_OK_PIN     GPIO_Pin_5

/* LED 短暂闪烁（任务上下文，可使用 vTaskDelay） */
static void led_pulse(GPIO_TypeDef *port, uint16_t pin, uint32_t ms)
{
    GPIO_ResetBits(port, pin);
    vTaskDelay(pdMS_TO_TICKS(ms));
    GPIO_SetBits(port, pin);
}

/* ════════════════════════════════════════════════════════════════
 *  命令处理器
 * ════════════════════════════════════════════════════════════════ */

static void handle_identify(const ParsedFrame_t *f)
{
    (void)f;
    /* 成功: 先灭错误灯，再亮成功灯 */
    GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
    uart_send_text_line("[OK] IDENTIFY: face recognized!");
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, NULL, 0);
    led_pulse(LED_OK_PORT, LED_OK_PIN, 2000);
}

static void handle_unknown(const ParsedFrame_t *f)
{
    (void)f;
    /* 未注册人脸: 错误灯闪烁 */
    GPIO_SetBits(LED_OK_PORT, LED_OK_PIN);
    uart_send_text_line("[WARN] UNKNOWN face detected!");
    led_pulse(LED_ERR_PORT, LED_ERR_PIN, 800);
}

static void handle_noface(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[INFO] NOFACE - standby");
}

static void handle_multiface(const ParsedFrame_t *f)
{
    (void)f;
    /* 多人脸: 错误灯闪烁 */
    GPIO_SetBits(LED_OK_PORT, LED_OK_PIN);
    uart_send_text_line("[WARN] MULTIFACE: multiple faces detected!");
    led_pulse(LED_ERR_PORT, LED_ERR_PIN, 800);
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
        /* 心跳恢复: 灭掉错误灯 */
        GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
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
