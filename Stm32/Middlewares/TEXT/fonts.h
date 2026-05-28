/* 字库系统 — 从 SPI Flash 读取 GBK 字库 */
#ifndef __FONTS_H
#define __FONTS_H

#include "stm32f10x.h"

/* 字库在 Flash 的起始地址 (12MB) */
#define FONTINFOADDR  (12UL * 1024UL * 1024UL)

/* 字库信息头 (33 字节, packed) */
typedef struct __attribute__((packed)) {
    uint8_t  fontok;       /* 0xAA = 字库有效 */
    uint32_t ugbkaddr;
    uint32_t ugbksize;
    uint32_t f12addr;
    uint32_t gbk12size;
    uint32_t f16addr;
    uint32_t gbk16size;
    uint32_t f24addr;
    uint32_t gbk24size;
} _font_info;

extern _font_info ftinfo;

uint8_t fonts_init(void);

#endif
