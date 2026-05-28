/* LED 模块 — PB5(红/失败) PE5(绿/成功), FreeRTOS 任务上下文调用 */
#ifndef __BSP_LED_H
#define __BSP_LED_H

#include "stm32f10x.h"

void led_init(void);
void led_boot_flash(void);        /* 启动: 两灯同闪 3 下 */
void led_success(void);           /* 成功: PE5 快闪 3 下 */
void led_failure(void);           /* 失败: PB5 快闪 3 下 */
void led_timeout_start(void);     /* 超时: PB5 持续闪 */
void led_timeout_stop(void);      /* 停止超时闪烁 */

/* 心跳定时器回调中调用, 驱动持续闪烁 */
void led_timeout_tick(void);

#endif
