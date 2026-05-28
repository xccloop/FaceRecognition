/* W25Q128 NOR Flash 驱动 (SPL) — PB12=CS, SPI2 */
#ifndef __NORFLASH_H
#define __NORFLASH_H

#include "stm32f10x.h"

#define NORFLASH_CS_PORT   GPIOB
#define NORFLASH_CS_PIN    GPIO_Pin_12

#define NORFLASH_CS(x)    do{ if(x) GPIO_SetBits(NORFLASH_CS_PORT, NORFLASH_CS_PIN); \
                               else  GPIO_ResetBits(NORFLASH_CS_PORT, NORFLASH_CS_PIN); \
                           }while(0)

extern uint16_t norflash_TYPE;

/* 命令 */
#define FLASH_WriteEnable      0x06
#define FLASH_ReadStatusReg1   0x05
#define FLASH_ReadData         0x03
#define FLASH_PageProgram      0x02
#define FLASH_SectorErase      0x20
#define FLASH_ChipErase        0xC7
#define FLASH_ManufactDeviceID 0x90
#define FLASH_JedecDeviceID    0x9F

void norflash_init(void);
uint16_t norflash_read_id(void);
void norflash_read(uint8_t *pbuf, uint32_t addr, uint16_t datalen);
void norflash_write(uint8_t *pbuf, uint32_t addr, uint16_t datalen);
void norflash_erase_sector(uint32_t saddr);
void norflash_erase_chip(void);

#endif
