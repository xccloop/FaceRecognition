/* ================================================================
 *  lcd.h — TFTLCD FSMC 驱动 (SPL 版本, 适配战舰V4)
 *
 *  支持 ILI9341 / NT35310 / NT35510 等 MCU 屏
 *  默认分辨率: 320×240 (ILI9341 横屏)
 * ================================================================ */

#ifndef LCD_H
#define LCD_H

#include "stm32f10x.h"
#include <stdint.h>

/* ── LCD 分辨率 (由 lcd_init 根据驱动 IC 自动设置) ── */
#define LCD_WIDTH   lcddev.width
#define LCD_HEIGHT  lcddev.height

/* ── FSMC 地址映射 (Bank1 区域4, A10=RS) ── */
#define LCD_FSMC_NEX  4
#define LCD_FSMC_AX   10

/* 基地址: Bank1=0x60000000, 每个 Bank 64MB, NE4 → +0x0C000000
 * RS 偏移: HADDR[25:1]→FSMC_A[24:0], 16位宽时 A10→HADDR[11], 偏移=(1<<11)=0x800
 * 使用 struct 方式: REG 偏移 0, RAM 偏移 (1<<(A10+1)) = 0x800 */
typedef struct {
    volatile uint16_t REG;   /* RS=0: 命令寄存器 */
    volatile uint16_t RAM;   /* RS=1: 数据寄存器 */
} LCD_TypeDef;

/* 基地址 + (1<<A10)*2-2 偏移: 确保 REG(RS=0) 和 RAM(RS=1) 分别落在 FSMC_A10 两侧 */
#define LCD_BASE  ((uint32_t)((0x60000000 + (0x04000000 * ((LCD_FSMC_NEX) - 1))) \
                   | (((1 << LCD_FSMC_AX) * 2) - 2)))
#define LCD       ((LCD_TypeDef *)LCD_BASE)

/* ── 颜色定义 (RGB565) ── */
#define WHITE       0xFFFF
#define BLACK       0x0000
#define BLUE        0x001F
#define RED         0xF800
#define MAGENTA     0xF81F
#define GREEN       0x07E0
#define CYAN        0x07FF
#define YELLOW      0xFFE0
#define GRAY        0x8430
#define DARKBLUE    0x01CF
#define LIGHTBLUE   0x7D7C
#define LIGHTGREEN  0x841F
#define LGRAY       0xC618
#define BROWN       0xBC40
#define BRRED       0xFC07
#define GRAYBLUE    0x5458

/* ── 扫描方向 ── */
#define L2R_U2D  0   /* 从左到右,从上到下 */
#define L2R_D2U  1
#define R2L_U2D  2
#define R2L_D2U  3
#define U2D_L2R  4
#define U2D_R2L  5
#define D2U_L2R  6
#define D2U_R2L  7

#define DFT_SCAN_DIR  L2R_U2D

/* ── LCD 设备描述符 ── */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t id;           /* 驱动IC ID: 0x9341, 0x5310, 0x5510, 0x1963 ... */
    uint8_t  dir;          /* 0=竖屏, 1=横屏 */
    uint16_t wramcmd;      /* 写GRAM命令 (通常 0x2C) */
    uint16_t setxcmd;      /* 设X坐标命令 (通常 0x2A) */
    uint16_t setycmd;      /* 设Y坐标命令 (通常 0x2B) */
} LcdDev_t;

extern LcdDev_t lcddev;

/* ── 全局颜色 ── */
extern uint16_t g_point_color;
extern uint16_t g_back_color;

/* ════════════════════════════════════════════════════════════════
 *  API
 * ════════════════════════════════════════════════════════════════ */

/* 初始化 */
void lcd_init(void);
void lcd_display_on(void);
void lcd_display_off(void);
void lcd_display_dir(uint8_t dir);
void lcd_scan_dir(uint8_t dir);

/* 背光 (0~100) */
void lcd_bl_set(uint8_t brightness);

/* 基础操作 */
void lcd_write_reg(uint16_t reg, uint16_t data);
void lcd_write_ram_prepare(void);
void lcd_set_cursor(uint16_t x, uint16_t y);
void lcd_set_window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height);

/* 绘制 */
void lcd_clear(uint16_t color);
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

/* 文本 — ASCII (8x16 / 12x12 / 16x16 / 24x24 / 32x32) */
void lcd_show_char(uint16_t x, uint16_t y, char chr, uint8_t size, uint8_t mode, uint16_t color);
void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                     uint8_t size, const char *p, uint16_t color);
void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
                  uint8_t size, uint16_t color);

#endif /* LCD_H */
