/* 文本渲染 — 混合中英文显示, 字库来自 SPI Flash */
#ifndef __TEXT_H
#define __TEXT_H

#include "stm32f10x.h"

void text_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      char *str, uint8_t size, uint8_t mode, uint16_t color);
void text_show_string_center(uint16_t x, uint16_t y, char *str, uint8_t size,
                             uint16_t width, uint16_t color);

#endif
