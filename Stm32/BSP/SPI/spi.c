/* SPI2 驱动 (SPL) — 适配 W25Q128, 模式3, MSB */
#include "spi.h"

void spi2_init(void)
{
    GPIO_InitTypeDef  g;
    SPI_InitTypeDef   s;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    /* SCK=PB13, MOSI=PB15: 复用推挽 */
    g.GPIO_Pin   = SPI2_SCK_PIN | SPI2_MOSI_PIN;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &g);

    /* MISO=PB14: 浮空输入 */
    g.GPIO_Pin   = SPI2_MISO_PIN;
    g.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &g);

    /* SPI2: 主模式, 模式3 (CPOL=1 CPHA=1), MSB, 预分频256(低速安全) */
    s.SPI_Direction      = SPI_Direction_2Lines_FullDuplex;
    s.SPI_Mode           = SPI_Mode_Master;
    s.SPI_DataSize       = SPI_DataSize_8b;
    s.SPI_CPOL           = SPI_CPOL_High;
    s.SPI_CPHA           = SPI_CPHA_2Edge;
    s.SPI_NSS            = SPI_NSS_Soft;
    s.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;
    s.SPI_FirstBit       = SPI_FirstBit_MSB;
    s.SPI_CRCPolynomial  = 7;
    SPI_Init(SPI2, &s);
    SPI_Cmd(SPI2, ENABLE);
}

void spi2_set_speed(uint8_t speed)
{
    uint16_t prescaler;
    switch (speed) {
    case SPI_SPEED_2:   prescaler = SPI_BaudRatePrescaler_2;   break;
    case SPI_SPEED_4:   prescaler = SPI_BaudRatePrescaler_4;   break;
    case SPI_SPEED_8:   prescaler = SPI_BaudRatePrescaler_8;   break;
    case SPI_SPEED_16:  prescaler = SPI_BaudRatePrescaler_16;  break;
    case SPI_SPEED_32:  prescaler = SPI_BaudRatePrescaler_32;  break;
    case SPI_SPEED_64:  prescaler = SPI_BaudRatePrescaler_64;  break;
    case SPI_SPEED_128: prescaler = SPI_BaudRatePrescaler_128; break;
    case SPI_SPEED_256: prescaler = SPI_BaudRatePrescaler_256; break;
    default: return;
    }
    SPI_Cmd(SPI2, DISABLE);
    SPI2->CR1 = (SPI2->CR1 & ~0x0038) | (prescaler << 3);
    SPI_Cmd(SPI2, ENABLE);
}

uint8_t spi2_read_write_byte(uint8_t txdata)
{
    while (!(SPI2->SR & SPI_I2S_FLAG_TXE));
    SPI2->DR = txdata;
    while (!(SPI2->SR & SPI_I2S_FLAG_RXNE));
    return (uint8_t)SPI2->DR;
}
