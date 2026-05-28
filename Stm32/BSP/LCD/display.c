/* 人脸识别结果 TFTLCD 显示 — SPI Flash 字库, GBK 编码 */
#include "display.h"
#include "lcd.h"
#include "../../Middlewares/TEXT/text.h"
#include <string.h>

/* ── 颜色 ── */
#define COLOR_BG        BLACK
#define COLOR_TITLE     WHITE
#define COLOR_SUCCESS   GREEN
#define COLOR_FAIL      RED
#define COLOR_INFO      CYAN
#define COLOR_WARN      YELLOW
#define COLOR_NAME      LIGHTBLUE

/* ── 布局 ── */
#define CENTER_Y     160
#define HEADER_Y     (CENTER_Y - 140)
#define BODY_Y       (CENTER_Y - 20)
#define FOOTER_Y     (CENTER_Y + 80)

/* ── GBK 编码汉字字符串 (避免 UTF-8 导致 Keil 编译错误) ── */

/* 系统就绪 */
static const char SYS_READY[] = {"\xCF\xB5\xCD\xB3\xBE\xCD\xD0\xF7"};
/* 识别成功 */
static const char IDENTIFY_OK[] = {"\xCA\xB6\xB1\xF0\xB3\xC9\xB9\xA6"};
/* 识别失败 */
static const char IDENTIFY_FAIL[] = {"\xCA\xB6\xB1\xF0\xCA\xA7\xB0\xDC"};
/* 欢迎,  */
static const char WELCOME_PREFIX[] = {"\xBB\xB6\xD3\xAD\x2C\x20"};
/* 未注册 */
static const char UNREGISTERED[] = {"\xCE\xB4\xD7\xA2\xB2\xE1"};
/* 多人脸 */
static const char MULTIFACE[] = {"\xB6\xE0\xC8\xCB\xC1\xB3"};
/* 无人脸 */
static const char NOFACE[] = {"\xCE\xDE\xC8\xCB\xC1\xB3"};
/* 待机中... */
static const char STANDBY[] = {"\xB4\xFD\xBB\xFA\xD6\xD0\x2E\x2E\x2E"};
/* 连接超时 */
static const char TIMEOUT[] = {"\xC1\xAC\xBD\xD3\xB3\xAC\xCA\xB1"};
/* 错误 */
static const char ERROR_STR[] = {"\xB4\xED\xCE\xF3"};
/* 等待树莓派连接... */
static const char WAIT_PI[] = {"\xB5\xC8\xB4\xFD\xCA\xF7\xDD\xAE\xC5\xC9\xC1\xAC\xBD\xD3\x2E\x2E\x2E"};

/* ── 辅助 ── */

static void draw_text_centered(uint16_t y, const char *s, uint8_t size, uint16_t color)
{
    /* 给 30px 左边距, 视觉上往左偏移 */
    text_show_string_center(0, y, (char *)s, size, LCD_WIDTH - 80, color);
}

static void draw_separator(uint16_t y, uint16_t color)
{
    lcd_draw_line(20, y, LCD_WIDTH - 20, y, color);
}

static void clear_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    lcd_fill(x, y, x + w - 1, y + h - 1, COLOR_BG);
}

/* ════════════════════════════════════════════════════════════════
 *  API
 * ════════════════════════════════════════════════════════════════ */

void display_init(void)
{
    g_back_color  = COLOR_BG;
    g_point_color = COLOR_TITLE;
    lcd_init();
    display_show_boot();
}

void display_show_boot(void)
{
    lcd_clear(BLACK);
    draw_text_centered(HEADER_Y,      "FaceRecognition",          16, CYAN);
    draw_text_centered(HEADER_Y + 40, SYS_READY,                 16, GREEN);
    draw_text_centered(HEADER_Y + 70, "STM32 v1.0",               16, LGRAY);
    draw_text_centered(FOOTER_Y,      WAIT_PI,                   16, GRAY);
}

static void draw_full_separator(uint16_t y, uint16_t color)
{
    lcd_draw_line(0, y, LCD_WIDTH - 1, y, color);
}

void display_show_standby(void)
{
    lcd_clear(BLACK);
    draw_text_centered(CENTER_Y, STANDBY, 16, COLOR_INFO);
}

void display_show_success(const char *name)
{
    char buf[64];
    uint16_t i = 0, j;
    uint16_t name_len = (uint16_t)strlen(name);

    lcd_clear(BLACK);
    draw_text_centered(CENTER_Y - 30, IDENTIFY_OK, 16, COLOR_SUCCESS);
    draw_full_separator(CENTER_Y, GREEN);

    /* "欢迎, NAME" */
    {
        const char *p = WELCOME_PREFIX;
        while (*p) buf[i++] = *p++;
    }
    if (name_len > 0 && name_len < 40) {
        for (j = 0; j < name_len && i < 60; j++) buf[i++] = name[j];
    } else {
        buf[i++] = '?'; buf[i++] = '?'; buf[i++] = '?';
    }
    buf[i] = '\0';
    draw_text_centered(CENTER_Y + 30, buf, 16, COLOR_NAME);

    lcd_fill(0, FOOTER_Y + 6, LCD_WIDTH - 1, FOOTER_Y + 10, GREEN);
}

void display_show_failure(DisplayReason_t reason)
{
    const char *reason_str = ERROR_STR;

    lcd_clear(BLACK);
    draw_text_centered(CENTER_Y - 30, IDENTIFY_FAIL, 16, COLOR_FAIL);
    draw_full_separator(CENTER_Y, RED);

    switch (reason) {
    case REASON_MULTIFACE:    reason_str = MULTIFACE;     break;
    case REASON_UNREGISTERED: reason_str = UNREGISTERED;  break;
    case REASON_NOFACE:       reason_str = NOFACE;        break;
    case REASON_TIMEOUT:      reason_str = TIMEOUT;       break;
    default:                                              break;
    }
    draw_text_centered(CENTER_Y + 30, reason_str, 16, COLOR_FAIL);
    lcd_fill(0, FOOTER_Y + 6, LCD_WIDTH - 1, FOOTER_Y + 10, RED);
}
