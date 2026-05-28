/* 文本渲染 — 从 SPI Flash 读 GBK 字库, 混合中英文显示 */
#include <string.h>
#include "text.h"
#include "fonts.h"
#include "../../BSP/LCD/lcd.h"
#include "../../BSP/NORFLASH/norflash.h"

/* 静态缓冲区 — 最大 72 字节 (24x24 字模) */
static uint8_t glyph_buf[72];

/* 从 Flash 读取单个汉字的字模数据 */
static void text_get_glyph(unsigned char *code, unsigned char *mat, uint8_t size)
{
    unsigned char qh, ql;
    unsigned long foffset;
    uint8_t csize = (size / 8 + ((size % 8) ? 1 : 0)) * size;
    uint8_t i;

    qh = *code;
    ql = *(code + 1);

    if (qh < 0x81 || ql < 0x40 || ql == 0xff || qh == 0xff) {
        for (i = 0; i < csize; i++) mat[i] = 0x00;
        return;
    }

    if (ql < 0x7f) ql -= 0x40;
    else           ql -= 0x41;

    qh -= 0x81;
    foffset = ((unsigned long)190 * qh + ql) * csize;

    switch (size) {
    case 12: norflash_read(mat, foffset + ftinfo.f12addr, csize); break;
    case 16: norflash_read(mat, foffset + ftinfo.f16addr, csize); break;
    case 24: norflash_read(mat, foffset + ftinfo.f24addr, csize); break;
    default:  for (i = 0; i < csize; i++) mat[i] = 0x00; break;
    }
}

/* 显示单个汉字 (GBK 编码, 2字节) */
static void text_show_font(uint16_t x, uint16_t y, uint8_t *font, uint8_t size,
                           uint8_t mode, uint16_t color)
{
    uint8_t temp, t, t1;
    uint16_t y0 = y;
    uint8_t csize = (size / 8 + ((size % 8) ? 1 : 0)) * size;

    if (size != 12 && size != 16 && size != 24) return;

    text_get_glyph(font, glyph_buf, size);

    for (t = 0; t < csize; t++) {
        temp = glyph_buf[t];
        for (t1 = 0; t1 < 8; t1++) {
            if (temp & 0x80)
                lcd_draw_point(x, y, color);
            else if (mode == 0)
                lcd_draw_point(x, y, g_back_color);
            temp <<= 1;
            y++;
            if ((y - y0) == size) {
                y = y0;
                x++;
                break;
            }
        }
    }
}

/* 混合中英文显示 (自动识别 GBK/ASCII) */
void text_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      char *str, uint8_t size, uint8_t mode, uint16_t color)
{
    uint16_t x0 = x;
    uint16_t y0 = y;
    uint8_t bHz = 0;
    uint8_t *pstr = (uint8_t *)str;

    while (*pstr) {
        if (!bHz) {
            if (*pstr > 0x80) {        /* GBK 汉字首字节 */
                bHz = 1;
            } else {                   /* ASCII */
                if (x > (x0 + width - size / 2)) { y += size; x = x0; }
                if (y > (y0 + height - size)) break;
                if (*pstr == '\n') { y += size; x = x0; pstr++; }
                else {
                    lcd_show_char(x, y, (char)*pstr, size, mode, color);
                    pstr++;
                    x += size / 2;
                }
            }
        } else {                       /* GBK 汉字第二字节 */
            bHz = 0;
            if (x > (x0 + width - size)) { y += size; x = x0; }
            if (y > (y0 + height - size)) break;
            text_show_font(x, y, pstr, size, mode, color);
            pstr += 2;
            x += size;
        }
    }
}

/* 居中显示 */
/* 计算字符串像素宽度: 中文=size, ASCII=size/2 */
static uint16_t str_pixel_width(const char *str, uint8_t size)
{
    uint16_t w = 0;
    uint8_t *p = (uint8_t *)str;
    while (*p) {
        if (*p > 0x80) { w += size; p += 2; }   /* GBK 汉字 */
        else           { w += size / 2; p++; }   /* ASCII */
    }
    return w;
}

void text_show_string_center(uint16_t x, uint16_t y, char *str, uint8_t size,
                             uint16_t width, uint16_t color)
{
    uint16_t pw = str_pixel_width(str, size);
    uint16_t sx;

    if (pw >= width)
        sx = x;
    else
        sx = x + (width - pw) / 2;

    text_show_string(sx, y, width, 999, (char *)str, size, 1, color);
}
