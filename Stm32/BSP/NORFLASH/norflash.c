/* W25Q128 NOR Flash 驱动 (SPL, 精简版 — 只保留读取功能) */
#include "norflash.h"
#include "../SPI/spi.h"

uint16_t norflash_TYPE = 0xFFFF;

static void norflash_write_enable(void)
{
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_WriteEnable);
    NORFLASH_CS(1);
}

static void norflash_wait_busy(void)
{
    uint8_t sr;
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_ReadStatusReg1);
    do {
        sr = spi2_read_write_byte(0xFF);
    } while (sr & 0x01);
    NORFLASH_CS(1);
}

static void norflash_send_address(uint32_t addr)
{
    spi2_read_write_byte((uint8_t)(addr >> 16));
    spi2_read_write_byte((uint8_t)(addr >> 8));
    spi2_read_write_byte((uint8_t)(addr));
}

void norflash_init(void)
{
    GPIO_InitTypeDef g;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    g.GPIO_Pin   = NORFLASH_CS_PIN;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(NORFLASH_CS_PORT, &g);
    NORFLASH_CS(1);

    spi2_init();
    spi2_set_speed(SPI_SPEED_2);
    norflash_TYPE = norflash_read_id();
}

uint16_t norflash_read_id(void)
{
    uint16_t id;
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_ManufactDeviceID);
    spi2_read_write_byte(0x00);
    spi2_read_write_byte(0x00);
    spi2_read_write_byte(0x00);
    id  = spi2_read_write_byte(0xFF) << 8;
    id |= spi2_read_write_byte(0xFF);
    NORFLASH_CS(1);
    return id;
}

void norflash_read(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_ReadData);
    norflash_send_address(addr);
    while (datalen--) {
        *pbuf++ = spi2_read_write_byte(0xFF);
    }
    NORFLASH_CS(1);
}

static void norflash_write_page(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    norflash_write_enable();
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_PageProgram);
    norflash_send_address(addr);
    while (datalen--) {
        spi2_read_write_byte(*pbuf++);
    }
    NORFLASH_CS(1);
    norflash_wait_busy();
}

static void norflash_write_nocheck(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    uint16_t remain = 256 - (addr & 0xFF);
    if (datalen <= remain) remain = datalen;
    while (1) {
        norflash_write_page(pbuf, addr, remain);
        if (datalen == remain) break;
        pbuf    += remain;
        addr    += remain;
        datalen -= remain;
        remain   = (datalen > 256) ? 256 : datalen;
    }
}

void norflash_write(uint8_t *pbuf, uint32_t addr, uint16_t datalen)
{
    uint32_t secpos;
    uint16_t secoff, remain;
    uint8_t  buf[256];

    secpos  = addr / 4096;
    secoff  = addr % 4096;
    remain  = 4096 - secoff;

    if (datalen <= remain) remain = datalen;

    while (1) {
        norflash_read(buf, secpos * 4096, 4096);
        {
            uint16_t i;
            for (i = 0; i < remain; i++) {
                if (buf[secoff + i] != 0xFF) break;
            }
            if (i != remain) {
                norflash_erase_sector(secpos);
                {
                    uint16_t j;
                    for (j = 0; j < remain; j++) buf[secoff + j] = pbuf[j];
                }
                norflash_write_nocheck(buf, secpos * 4096, 4096);
            } else {
                norflash_write_nocheck(pbuf, addr, remain);
            }
        }
        if (datalen == remain) break;
        secpos++;
        secoff = 0;
        pbuf  += remain;
        addr  += remain;
        datalen -= remain;
        remain = (datalen > 4096) ? 4096 : datalen;
    }
}

void norflash_erase_sector(uint32_t saddr)
{
    norflash_write_enable();
    norflash_wait_busy();
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_SectorErase);
    norflash_send_address(saddr * 4096);
    NORFLASH_CS(1);
    norflash_wait_busy();
}

void norflash_erase_chip(void)
{
    norflash_write_enable();
    norflash_wait_busy();
    NORFLASH_CS(0);
    spi2_read_write_byte(FLASH_ChipErase);
    NORFLASH_CS(1);
    norflash_wait_busy();
}
