/* ================================================================
 *  display.h — 人脸识别结果 TFTLCD 显示模块
 *
 *  职责: 将识别成功/失败/状态信息格式化到 LCD
 *  依赖: lcd.h (底层驱动)
 * ================================================================ */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* ── 显示原因码 (对应识别失败的具体原因) ── */
typedef enum {
    REASON_NONE          = 0,   /* 无错误 */
    REASON_UNREGISTERED  = 1,   /* 未注册人脸 */
    REASON_MULTIFACE     = 2,   /* 多人脸 */
    REASON_NOFACE        = 3,   /* 无人脸 */
    REASON_TIMEOUT       = 4,   /* Pi 心跳超时 */
    REASON_UNKNOWN       = 5,   /* 未知错误 */
} DisplayReason_t;

/* ── API ── */

/* 初始化显示 (含启动画面) */
void display_init(void);

/* 显示启动画面 (系统就绪) */
void display_show_boot(void);

/* 显示待机画面 */
void display_show_standby(void);

/* 识别成功: 显示 "识别成功，欢迎" + 名字 (ASCII) */
void display_show_success(const char *name);

/* 识别失败: 显示失败原因 (中文) */
void display_show_failure(DisplayReason_t reason);

#endif /* DISPLAY_H */
