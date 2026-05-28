/* SPI2 驱动 (SPL) — PB13=SCK PB14=MISO PB15=MOSI */
#ifndef __SPI_H
#define __SPI_H

#include "stm32f10x.h"

#define SPI2_SCK_PORT   GPIOB
#define SPI2_SCK_PIN    GPIO_Pin_13
#define SPI2_MISO_PORT  GPIOB
#define SPI2_MISO_PIN   GPIO_Pin_14
#define SPI2_MOSI_PORT  GPIOB
#define SPI2_MOSI_PIN   GPIO_Pin_15

#define SPI_SPEED_2     0
#define SPI_SPEED_4     1
#define SPI_SPEED_8     2
#define SPI_SPEED_16    3
#define SPI_SPEED_32    4
#define SPI_SPEED_64    5
#define SPI_SPEED_128   6
#define SPI_SPEED_256   7

void spi2_init(void);
void spi2_set_speed(uint8_t speed);
uint8_t spi2_read_write_byte(uint8_t txdata);

#endif
