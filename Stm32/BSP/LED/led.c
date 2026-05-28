/* LED 模块 — PB5=错误 PE5=成功, 使用 vTaskDelay */
#include "led.h"
#include "FreeRTOS.h"
#include "task.h"

#define LED_ERR_PORT  GPIOB
#define LED_ERR_PIN   GPIO_Pin_5
#define LED_OK_PORT   GPIOE
#define LED_OK_PIN    GPIO_Pin_5

static uint8_t timeout_blinking = 0;

void led_init(void)
{
    GPIO_InitTypeDef g;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOE, ENABLE);
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Pin   = LED_ERR_PIN;
    GPIO_Init(LED_ERR_PORT, &g);
    GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);   /* 灭 */
    g.GPIO_Pin   = LED_OK_PIN;
    GPIO_Init(LED_OK_PORT, &g);
    GPIO_SetBits(LED_OK_PORT, LED_OK_PIN);     /* 灭 */
}

void led_boot_flash(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);   /* PB5 亮 */
        GPIO_ResetBits(LED_OK_PORT,  LED_OK_PIN);    /* PE5 亮 */
        vTaskDelay(pdMS_TO_TICKS(150));
        GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);      /* 灭 */
        GPIO_SetBits(LED_OK_PORT,  LED_OK_PIN);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void led_flash(GPIO_TypeDef *port, uint16_t pin, int times)
{
    int i;
    for (i = 0; i < times; i++) {
        GPIO_ResetBits(port, pin);
        vTaskDelay(pdMS_TO_TICKS(100));
        GPIO_SetBits(port, pin);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void led_success(void)
{
    GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
    led_flash(LED_OK_PORT, LED_OK_PIN, 3);
}

void led_failure(void)
{
    GPIO_SetBits(LED_OK_PORT, LED_OK_PIN);
    led_flash(LED_ERR_PORT, LED_ERR_PIN, 3);
}

void led_timeout_start(void)
{
    timeout_blinking = 1;
}

void led_timeout_stop(void)
{
    timeout_blinking = 0;
    GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
}

void led_timeout_tick(void)
{
    if (timeout_blinking) {
        static uint8_t state = 0;
        state = !state;
        if (state)
            GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);
        else
            GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
    }
}
