/* 字库初始化 — 从 SPI Flash 读取字库信息头 */
#include "fonts.h"
#include "../../BSP/NORFLASH/norflash.h"

_font_info ftinfo;

uint8_t fonts_init(void)
{
    uint8_t t = 0;

    while (t < 10) {
        t++;
        norflash_read((uint8_t *)&ftinfo, FONTINFOADDR, sizeof(ftinfo));
        if (ftinfo.fontok == 0xAA) return 0;
        {
            volatile uint32_t d = 1200000; while (d--) __NOP();  /* ~20ms */
        }
    }
    return 1;  /* 字库丢失 */
}
