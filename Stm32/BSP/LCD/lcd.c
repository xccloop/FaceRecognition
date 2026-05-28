/* ================================================================
 *  lcd.c — TFTLCD FSMC 驱动实现 (SPL, 适配战舰V4)
 *
 *  移植自 ALIENTEK HAL 例程, 改为 SPL + FreeRTOS 上下文
 *  支持自动检测: ILI9341 / NT35310 / NT35510 / SSD1963
 * ================================================================ */

#include "lcd.h"
#include "font.h"

/* ── 全局变量 ── */
LcdDev_t lcddev;
uint16_t g_point_color = RED;
uint16_t g_back_color  = WHITE;

/* ── 背光 PWM 频率 ── */
#define LCD_BL_TIM_PERIOD  899    /* 72MHz / (0+1) / 900 = 80kHz ... 实际用900分频 */
#define LCD_BL_DEFAULT      80    /* 默认亮度 80% */

/* ════════════════════════════════════════════════════════════════
 *  低层寄存器访问
 * ════════════════════════════════════════════════════════════════ */

static inline void lcd_wr_reg(uint16_t reg)   { LCD->REG = reg; }
static inline void lcd_wr_data(uint16_t data) { LCD->RAM = data; }

static inline uint16_t lcd_rd_data(void)
{
    uint16_t ram;
    ram = LCD->RAM;
    return ram;
}

void lcd_write_reg(uint16_t reg, uint16_t data)
{
    LCD->REG = reg;
    LCD->RAM = data;
}

void lcd_write_ram_prepare(void)
{
    LCD->REG = lcddev.wramcmd;
}

/* ── 短延时 (~2 个 NOP) ── */
static void lcd_opt_delay(uint32_t n)
{
    while (n--) __NOP();
}

/* ── 毫秒级忙等延时 (调度器启动前也可用) ── */
static void lcd_delay_ms(uint32_t ms)
{
    uint32_t i;
    /* 72MHz → ~72000 周期/ms, 粗略延时 */
    for (; ms > 0; ms--) {
        for (i = 0; i < 12000; i++) {
            __NOP();
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 *  FSMC GPIO 初始化 (SPL)
 * ════════════════════════════════════════════════════════════════ */

static void FSMC_LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef g;

    /* 时钟: GPIOD, GPIOE, GPIOG + FSMC (AHB) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE |
                           RCC_APB2Periph_GPIOG, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC, ENABLE);

    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;

    /* PD0,1,4,5,8,9,10,14,15: FSMC_D2/D3/NOE/NWE/D13/D14/D15/D0/D1 */
    g.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5 |
                 GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 |
                 GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_Init(GPIOD, &g);

    /* PE7~15: FSMC_D4~D12 */
    g.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 |
                 GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_Init(GPIOE, &g);

    /* PG0=FSMC_A10(RS), PG12=FSMC_NE4(CS) */
    g.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_12;
    GPIO_Init(GPIOG, &g);
}

/* ════════════════════════════════════════════════════════════════
 *  FSMC 时序配置 (SPL — 读慢写快)
 * ════════════════════════════════════════════════════════════════ */

static void FSMC_LCD_Init(void)
{
    FSMC_NORSRAMInitTypeDef        fsmc;
    FSMC_NORSRAMTimingInitTypeDef  r_timing;  /* 读时序 */
    FSMC_NORSRAMTimingInitTypeDef  w_timing;  /* 写时序 */

    /* ── 读时序: 地址建立 0, 数据建立 15 HCLK (~222ns @72MHz) ── */
    r_timing.FSMC_AddressSetupTime      = 0x00;
    r_timing.FSMC_AddressHoldTime       = 0x00;
    r_timing.FSMC_DataSetupTime         = 0x0F;
    r_timing.FSMC_BusTurnAroundDuration = 0x00;
    r_timing.FSMC_CLKDivision           = 0x00;
    r_timing.FSMC_DataLatency           = 0x00;
    r_timing.FSMC_AccessMode            = FSMC_AccessMode_A;

    /* ── 写时序: 地址建立 0, 数据建立 1 HCLK (~27ns @72MHz) ── */
    w_timing.FSMC_AddressSetupTime      = 0x00;
    w_timing.FSMC_AddressHoldTime       = 0x00;
    w_timing.FSMC_DataSetupTime         = 0x01;
    w_timing.FSMC_BusTurnAroundDuration = 0x00;
    w_timing.FSMC_CLKDivision           = 0x00;
    w_timing.FSMC_DataLatency           = 0x00;
    w_timing.FSMC_AccessMode            = FSMC_AccessMode_A;

    /* ── FSMC NOR/SRAM Bank4 ── */
    fsmc.FSMC_Bank                  = FSMC_Bank1_NORSRAM4;
    fsmc.FSMC_DataAddressMux        = FSMC_DataAddressMux_Disable;
    fsmc.FSMC_MemoryType            = FSMC_MemoryType_SRAM;
    fsmc.FSMC_MemoryDataWidth       = FSMC_MemoryDataWidth_16b;
    fsmc.FSMC_BurstAccessMode       = FSMC_BurstAccessMode_Disable;
    fsmc.FSMC_AsynchronousWait      = FSMC_AsynchronousWait_Disable;
    fsmc.FSMC_WaitSignalPolarity    = FSMC_WaitSignalPolarity_Low;
    fsmc.FSMC_WrapMode              = FSMC_WrapMode_Disable;
    fsmc.FSMC_WaitSignalActive      = FSMC_WaitSignalActive_BeforeWaitState;
    fsmc.FSMC_WriteOperation        = FSMC_WriteOperation_Enable;
    fsmc.FSMC_WaitSignal            = FSMC_WaitSignal_Disable;
    fsmc.FSMC_ExtendedMode          = FSMC_ExtendedMode_Enable;   /* 读写独立时序 */
    fsmc.FSMC_WriteBurst            = FSMC_WriteBurst_Disable;
    fsmc.FSMC_ReadWriteTimingStruct = &r_timing;
    fsmc.FSMC_WriteTimingStruct     = &w_timing;

    FSMC_NORSRAMInit(&fsmc);
    FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM4, ENABLE);
}

/* ════════════════════════════════════════════════════════════════
 *  背光控制 (TIM3 CH3 → PB0 PWM)
 * ════════════════════════════════════════════════════════════════ */

void lcd_bl_set(uint8_t brightness)
{
    uint16_t pwm_val;
    if (brightness > 100) brightness = 100;
    pwm_val = (uint16_t)((uint32_t)brightness * (LCD_BL_TIM_PERIOD + 1) / 100);
    if (pwm_val > 0) pwm_val -= 1;  /* 避免 off-by-one */
    TIM_SetCompare3(TIM3, pwm_val);
}

static void LCD_BL_Init(void)
{
    GPIO_InitTypeDef        g;
    TIM_TimeBaseInitTypeDef tim;
    TIM_OCInitTypeDef       oc;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /* PB0 → TIM3_CH3 (默认映射, 无需 remap) */
    g.GPIO_Pin   = GPIO_Pin_0;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &g);

    /* 1kHz PWM: 72MHz / 72 / 1000 = 1kHz */
    tim.TIM_Prescaler         = 71;
    tim.TIM_Period            = 999;
    tim.TIM_ClockDivision     = 0;
    tim.TIM_CounterMode       = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim);

    oc.TIM_OCMode      = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse       = 0;
    oc.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC3Init(TIM3, &oc);

    TIM_Cmd(TIM3, ENABLE);
}

/* ════════════════════════════════════════════════════════════════
 *  LCD 驱动 IC 识别 & 初始化
 * ════════════════════════════════════════════════════════════════ */

static void lcd_id_read(void)
{
    /* ── 先试 ILI9341 (2.8寸) ── */
    lcd_wr_reg(0xD3);
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id <<= 8;
    lcddev.id |= lcd_rd_data();
    if (lcddev.id == 0x9341) return;

    /* ── 试 NT35310 (3.5寸) ── */
    lcd_wr_reg(0xD4);
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id <<= 8;
    lcddev.id |= lcd_rd_data();
    if (lcddev.id == 0x5310) return;

    /* ── 试 SSD1963 (7寸) ── */
    lcd_wr_reg(0xA1);
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id  = lcd_rd_data();
    lcddev.id <<= 8;
    lcddev.id |= lcd_rd_data();
    if (lcddev.id == 0x1963) return;

    /* 未知, 默认 9341 */
    lcddev.id = 0x9341;
}

/* ── ILI9341 初始化序列 ── */
static void LCD_ILI9341_InitSeq(void)
{
    /* 硬件复位: PG11 拉低 20ms 再拉高 (如果接有 RST 引脚) */

    lcd_write_reg(0xCF, 0x00);
    lcd_wr_data(0xC1);
    lcd_wr_data(0x30);

    lcd_write_reg(0xED, 0x64);
    lcd_wr_data(0x03);
    lcd_wr_data(0x12);
    lcd_wr_data(0x81);

    lcd_write_reg(0xE8, 0x85);
    lcd_wr_data(0x10);
    lcd_wr_data(0x7A);

    lcd_write_reg(0xCB, 0x39);
    lcd_wr_data(0x2C);
    lcd_wr_data(0x00);
    lcd_wr_data(0x34);
    lcd_wr_data(0x02);

    lcd_write_reg(0xF7, 0x20);

    lcd_write_reg(0xEA, 0x00);
    lcd_wr_data(0x00);

    lcd_write_reg(0xC0, 0x1B);          /* Power Control */

    lcd_write_reg(0xC1, 0x01);          /* Power Control */

    lcd_write_reg(0xC5, 0x30);          /* VCOM Control */
    lcd_wr_data(0x30);

    lcd_write_reg(0xC7, 0xB7);          /* VCOM Control2 */

    lcd_write_reg(0x36, 0x48);          /* Memory Access Control: MX + BGR */

    lcd_write_reg(0x3A, 0x55);          /* Pixel Format: 16-bit (RGB565) */

    lcd_write_reg(0xB1, 0x00);          /* Frame Rate Control */
    lcd_wr_data(0x1A);

    lcd_write_reg(0xB6, 0x0A);          /* Display Function Control */
    lcd_wr_data(0xA2);

    lcd_write_reg(0xF2, 0x00);          /* 3Gamma Function Disable */

    lcd_write_reg(0x26, 0x01);          /* Gamma curve selected */

    /* Positive Gamma Correction (15 bytes) */
    lcd_write_reg(0xE0, 0x0F);
    lcd_wr_data(0x31); lcd_wr_data(0x2B); lcd_wr_data(0x0C);
    lcd_wr_data(0x0E); lcd_wr_data(0x08); lcd_wr_data(0x4E);
    lcd_wr_data(0xF1); lcd_wr_data(0x37); lcd_wr_data(0x07);
    lcd_wr_data(0x10); lcd_wr_data(0x03); lcd_wr_data(0x0E);
    lcd_wr_data(0x09); lcd_wr_data(0x00);

    /* Negative Gamma Correction (15 bytes) */
    lcd_write_reg(0xE1, 0x00);
    lcd_wr_data(0x0E); lcd_wr_data(0x14); lcd_wr_data(0x03);
    lcd_wr_data(0x11); lcd_wr_data(0x07); lcd_wr_data(0x31);
    lcd_wr_data(0xC1); lcd_wr_data(0x48); lcd_wr_data(0x08);
    lcd_wr_data(0x0F); lcd_wr_data(0x0C); lcd_wr_data(0x31);
    lcd_wr_data(0x36); lcd_wr_data(0x0F);

    /* Sleep Out + 120ms delay */
    lcd_write_reg(0x11, 0x0000);
    lcd_delay_ms(120);

    /* Display ON */
    lcd_write_reg(0x29, 0x0000);
}

/* ── NT35310 (3.5寸 320x480) 初始化序列 (来自 ALIENTEK 官方 HAL 例程) ── */
static void LCD_NT35310_InitSeq(void)
{
    lcd_wr_reg(0xED); lcd_wr_data(0x01); lcd_wr_data(0xFE);
    lcd_wr_reg(0xEE); lcd_wr_data(0xDE); lcd_wr_data(0x21);
    lcd_wr_reg(0xF1); lcd_wr_data(0x01);
    lcd_wr_reg(0xDF); lcd_wr_data(0x10);

    lcd_wr_reg(0xC4); lcd_wr_data(0x8F);
    lcd_wr_reg(0xC6); lcd_wr_data(0x00); lcd_wr_data(0xE2); lcd_wr_data(0xE2); lcd_wr_data(0xE2);
    lcd_wr_reg(0xBF); lcd_wr_data(0xAA);

    lcd_wr_reg(0xB0);
    lcd_wr_data(0x0D); lcd_wr_data(0x00); lcd_wr_data(0x0D); lcd_wr_data(0x00);
    lcd_wr_data(0x11); lcd_wr_data(0x00); lcd_wr_data(0x19); lcd_wr_data(0x00);
    lcd_wr_data(0x21); lcd_wr_data(0x00); lcd_wr_data(0x2D); lcd_wr_data(0x00);
    lcd_wr_data(0x3D); lcd_wr_data(0x00); lcd_wr_data(0x5D); lcd_wr_data(0x00);
    lcd_wr_data(0x5D); lcd_wr_data(0x00);

    lcd_wr_reg(0xB1);
    lcd_wr_data(0x80); lcd_wr_data(0x00); lcd_wr_data(0x8B); lcd_wr_data(0x00);
    lcd_wr_data(0x96); lcd_wr_data(0x00);

    lcd_wr_reg(0xB2);
    lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x02); lcd_wr_data(0x00);
    lcd_wr_data(0x03); lcd_wr_data(0x00);

    { uint8_t _i; lcd_wr_reg(0xB3); for (_i=0; _i<24; _i++) lcd_wr_data(0x00); }

    lcd_wr_reg(0xB4);
    lcd_wr_data(0x8B); lcd_wr_data(0x00); lcd_wr_data(0x96); lcd_wr_data(0x00);
    lcd_wr_data(0xA1); lcd_wr_data(0x00);

    lcd_wr_reg(0xB5);
    lcd_wr_data(0x02); lcd_wr_data(0x00); lcd_wr_data(0x03); lcd_wr_data(0x00);
    lcd_wr_data(0x04); lcd_wr_data(0x00);

    lcd_wr_reg(0xB6); lcd_wr_data(0x00); lcd_wr_data(0x00);

    lcd_wr_reg(0xB7);
    lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x3F); lcd_wr_data(0x00);
    lcd_wr_data(0x5E); lcd_wr_data(0x00); lcd_wr_data(0x64); lcd_wr_data(0x00);
    lcd_wr_data(0x8C); lcd_wr_data(0x00); lcd_wr_data(0xAC); lcd_wr_data(0x00);
    lcd_wr_data(0xDC); lcd_wr_data(0x00); lcd_wr_data(0x70); lcd_wr_data(0x00);
    lcd_wr_data(0x90); lcd_wr_data(0x00); lcd_wr_data(0xEB); lcd_wr_data(0x00);
    lcd_wr_data(0xDC); lcd_wr_data(0x00);

    { uint8_t _i; lcd_wr_reg(0xB8); for (_i=0; _i<8; _i++) lcd_wr_data(0x00); }

    lcd_wr_reg(0xBA); lcd_wr_data(0x24); lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x00);

    lcd_wr_reg(0xC1);
    lcd_wr_data(0x20); lcd_wr_data(0x00); lcd_wr_data(0x54); lcd_wr_data(0x00);
    lcd_wr_data(0xFF); lcd_wr_data(0x00);

    lcd_wr_reg(0xC2); lcd_wr_data(0x0A); lcd_wr_data(0x00); lcd_wr_data(0x04); lcd_wr_data(0x00);

    lcd_wr_reg(0xC3);
    lcd_wr_data(0x3C); lcd_wr_data(0x00); lcd_wr_data(0x3A); lcd_wr_data(0x00);
    lcd_wr_data(0x39); lcd_wr_data(0x00); lcd_wr_data(0x37); lcd_wr_data(0x00);
    lcd_wr_data(0x3C); lcd_wr_data(0x00); lcd_wr_data(0x36); lcd_wr_data(0x00);
    lcd_wr_data(0x32); lcd_wr_data(0x00); lcd_wr_data(0x2F); lcd_wr_data(0x00);
    lcd_wr_data(0x2C); lcd_wr_data(0x00); lcd_wr_data(0x29); lcd_wr_data(0x00);
    lcd_wr_data(0x26); lcd_wr_data(0x00); lcd_wr_data(0x24); lcd_wr_data(0x00);
    lcd_wr_data(0x24); lcd_wr_data(0x00); lcd_wr_data(0x23); lcd_wr_data(0x00);
    lcd_wr_data(0x3C); lcd_wr_data(0x00); lcd_wr_data(0x36); lcd_wr_data(0x00);
    lcd_wr_data(0x32); lcd_wr_data(0x00); lcd_wr_data(0x2F); lcd_wr_data(0x00);
    lcd_wr_data(0x2C); lcd_wr_data(0x00); lcd_wr_data(0x29); lcd_wr_data(0x00);
    lcd_wr_data(0x26); lcd_wr_data(0x00); lcd_wr_data(0x24); lcd_wr_data(0x00);
    lcd_wr_data(0x24); lcd_wr_data(0x00); lcd_wr_data(0x23); lcd_wr_data(0x00);

    lcd_wr_reg(0xC4);
    lcd_wr_data(0x62); lcd_wr_data(0x00); lcd_wr_data(0x05); lcd_wr_data(0x00);
    lcd_wr_data(0x84); lcd_wr_data(0x00); lcd_wr_data(0xF0); lcd_wr_data(0x00);
    lcd_wr_data(0x18); lcd_wr_data(0x00); lcd_wr_data(0xA4); lcd_wr_data(0x00);
    lcd_wr_data(0x18); lcd_wr_data(0x00); lcd_wr_data(0x50); lcd_wr_data(0x00);
    lcd_wr_data(0x0C); lcd_wr_data(0x00); lcd_wr_data(0x17); lcd_wr_data(0x00);
    lcd_wr_data(0x95); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);
    lcd_wr_data(0xE6); lcd_wr_data(0x00);

    lcd_wr_reg(0xC5);
    lcd_wr_data(0x32); lcd_wr_data(0x00); lcd_wr_data(0x44); lcd_wr_data(0x00);
    lcd_wr_data(0x65); lcd_wr_data(0x00); lcd_wr_data(0x76); lcd_wr_data(0x00);
    lcd_wr_data(0x88); lcd_wr_data(0x00);

    lcd_wr_reg(0xC6); lcd_wr_data(0x20); lcd_wr_data(0x00); lcd_wr_data(0x17); lcd_wr_data(0x00);
    lcd_wr_data(0x01); lcd_wr_data(0x00);

    { uint8_t _i; lcd_wr_reg(0xC7); for (_i=0; _i<4; _i++) lcd_wr_data(0x00); }
    { uint8_t _i; lcd_wr_reg(0xC8); for (_i=0; _i<4; _i++) lcd_wr_data(0x00); }
    { uint8_t _i; lcd_wr_reg(0xC9); for (_i=0; _i<16; _i++) lcd_wr_data(0x00); }

    /* Gamma E0 */
    lcd_wr_reg(0xE0);
    lcd_wr_data(0x16); lcd_wr_data(0x00); lcd_wr_data(0x1C); lcd_wr_data(0x00);
    lcd_wr_data(0x21); lcd_wr_data(0x00); lcd_wr_data(0x36); lcd_wr_data(0x00);
    lcd_wr_data(0x46); lcd_wr_data(0x00); lcd_wr_data(0x52); lcd_wr_data(0x00);
    lcd_wr_data(0x64); lcd_wr_data(0x00); lcd_wr_data(0x7A); lcd_wr_data(0x00);
    lcd_wr_data(0x8B); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0xA8); lcd_wr_data(0x00); lcd_wr_data(0xB9); lcd_wr_data(0x00);
    lcd_wr_data(0xC4); lcd_wr_data(0x00); lcd_wr_data(0xCA); lcd_wr_data(0x00);
    lcd_wr_data(0xD2); lcd_wr_data(0x00); lcd_wr_data(0xD9); lcd_wr_data(0x00);
    lcd_wr_data(0xE0); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E1 */
    lcd_wr_reg(0xE1);
    lcd_wr_data(0x16); lcd_wr_data(0x00); lcd_wr_data(0x1C); lcd_wr_data(0x00);
    lcd_wr_data(0x22); lcd_wr_data(0x00); lcd_wr_data(0x36); lcd_wr_data(0x00);
    lcd_wr_data(0x45); lcd_wr_data(0x00); lcd_wr_data(0x52); lcd_wr_data(0x00);
    lcd_wr_data(0x64); lcd_wr_data(0x00); lcd_wr_data(0x7A); lcd_wr_data(0x00);
    lcd_wr_data(0x8B); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0xA8); lcd_wr_data(0x00); lcd_wr_data(0xB9); lcd_wr_data(0x00);
    lcd_wr_data(0xC4); lcd_wr_data(0x00); lcd_wr_data(0xCA); lcd_wr_data(0x00);
    lcd_wr_data(0xD2); lcd_wr_data(0x00); lcd_wr_data(0xD8); lcd_wr_data(0x00);
    lcd_wr_data(0xE0); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E2 */
    lcd_wr_reg(0xE2);
    lcd_wr_data(0x05); lcd_wr_data(0x00); lcd_wr_data(0x0B); lcd_wr_data(0x00);
    lcd_wr_data(0x1B); lcd_wr_data(0x00); lcd_wr_data(0x34); lcd_wr_data(0x00);
    lcd_wr_data(0x44); lcd_wr_data(0x00); lcd_wr_data(0x4F); lcd_wr_data(0x00);
    lcd_wr_data(0x61); lcd_wr_data(0x00); lcd_wr_data(0x79); lcd_wr_data(0x00);
    lcd_wr_data(0x88); lcd_wr_data(0x00); lcd_wr_data(0x97); lcd_wr_data(0x00);
    lcd_wr_data(0xA6); lcd_wr_data(0x00); lcd_wr_data(0xB7); lcd_wr_data(0x00);
    lcd_wr_data(0xC2); lcd_wr_data(0x00); lcd_wr_data(0xC7); lcd_wr_data(0x00);
    lcd_wr_data(0xD1); lcd_wr_data(0x00); lcd_wr_data(0xD6); lcd_wr_data(0x00);
    lcd_wr_data(0xDD); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E3 */
    lcd_wr_reg(0xE3);
    lcd_wr_data(0x05); lcd_wr_data(0x00); lcd_wr_data(0x0A); lcd_wr_data(0x00);
    lcd_wr_data(0x1C); lcd_wr_data(0x00); lcd_wr_data(0x33); lcd_wr_data(0x00);
    lcd_wr_data(0x44); lcd_wr_data(0x00); lcd_wr_data(0x50); lcd_wr_data(0x00);
    lcd_wr_data(0x62); lcd_wr_data(0x00); lcd_wr_data(0x78); lcd_wr_data(0x00);
    lcd_wr_data(0x88); lcd_wr_data(0x00); lcd_wr_data(0x97); lcd_wr_data(0x00);
    lcd_wr_data(0xA6); lcd_wr_data(0x00); lcd_wr_data(0xB7); lcd_wr_data(0x00);
    lcd_wr_data(0xC2); lcd_wr_data(0x00); lcd_wr_data(0xC7); lcd_wr_data(0x00);
    lcd_wr_data(0xD1); lcd_wr_data(0x00); lcd_wr_data(0xD5); lcd_wr_data(0x00);
    lcd_wr_data(0xDD); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E4 */
    lcd_wr_reg(0xE4);
    lcd_wr_data(0x01); lcd_wr_data(0x00); lcd_wr_data(0x01); lcd_wr_data(0x00);
    lcd_wr_data(0x02); lcd_wr_data(0x00); lcd_wr_data(0x2A); lcd_wr_data(0x00);
    lcd_wr_data(0x3C); lcd_wr_data(0x00); lcd_wr_data(0x4B); lcd_wr_data(0x00);
    lcd_wr_data(0x5D); lcd_wr_data(0x00); lcd_wr_data(0x74); lcd_wr_data(0x00);
    lcd_wr_data(0x84); lcd_wr_data(0x00); lcd_wr_data(0x93); lcd_wr_data(0x00);
    lcd_wr_data(0xA2); lcd_wr_data(0x00); lcd_wr_data(0xB3); lcd_wr_data(0x00);
    lcd_wr_data(0xBE); lcd_wr_data(0x00); lcd_wr_data(0xC4); lcd_wr_data(0x00);
    lcd_wr_data(0xCD); lcd_wr_data(0x00); lcd_wr_data(0xD3); lcd_wr_data(0x00);
    lcd_wr_data(0xDD); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E5 */
    lcd_wr_reg(0xE5);
    lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x00);
    lcd_wr_data(0x02); lcd_wr_data(0x00); lcd_wr_data(0x29); lcd_wr_data(0x00);
    lcd_wr_data(0x3C); lcd_wr_data(0x00); lcd_wr_data(0x4B); lcd_wr_data(0x00);
    lcd_wr_data(0x5D); lcd_wr_data(0x00); lcd_wr_data(0x74); lcd_wr_data(0x00);
    lcd_wr_data(0x84); lcd_wr_data(0x00); lcd_wr_data(0x93); lcd_wr_data(0x00);
    lcd_wr_data(0xA2); lcd_wr_data(0x00); lcd_wr_data(0xB3); lcd_wr_data(0x00);
    lcd_wr_data(0xBE); lcd_wr_data(0x00); lcd_wr_data(0xC4); lcd_wr_data(0x00);
    lcd_wr_data(0xCD); lcd_wr_data(0x00); lcd_wr_data(0xD3); lcd_wr_data(0x00);
    lcd_wr_data(0xDC); lcd_wr_data(0x00); lcd_wr_data(0xF3); lcd_wr_data(0x00);

    /* Gamma E6 */
    lcd_wr_reg(0xE6);
    lcd_wr_data(0x11); lcd_wr_data(0x00); lcd_wr_data(0x34); lcd_wr_data(0x00);
    lcd_wr_data(0x56); lcd_wr_data(0x00); lcd_wr_data(0x76); lcd_wr_data(0x00);
    lcd_wr_data(0x77); lcd_wr_data(0x00); lcd_wr_data(0x66); lcd_wr_data(0x00);
    lcd_wr_data(0x88); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0xBB); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0x66); lcd_wr_data(0x00); lcd_wr_data(0x55); lcd_wr_data(0x00);
    lcd_wr_data(0x55); lcd_wr_data(0x00); lcd_wr_data(0x45); lcd_wr_data(0x00);
    lcd_wr_data(0x43); lcd_wr_data(0x00); lcd_wr_data(0x44); lcd_wr_data(0x00);

    /* Gamma E7 */
    lcd_wr_reg(0xE7);
    lcd_wr_data(0x32); lcd_wr_data(0x00); lcd_wr_data(0x55); lcd_wr_data(0x00);
    lcd_wr_data(0x76); lcd_wr_data(0x00); lcd_wr_data(0x66); lcd_wr_data(0x00);
    lcd_wr_data(0x67); lcd_wr_data(0x00); lcd_wr_data(0x67); lcd_wr_data(0x00);
    lcd_wr_data(0x87); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0xBB); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0x77); lcd_wr_data(0x00); lcd_wr_data(0x44); lcd_wr_data(0x00);
    lcd_wr_data(0x56); lcd_wr_data(0x00); lcd_wr_data(0x23); lcd_wr_data(0x00);
    lcd_wr_data(0x33); lcd_wr_data(0x00); lcd_wr_data(0x45); lcd_wr_data(0x00);

    /* Gamma E8 */
    lcd_wr_reg(0xE8);
    lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0x87); lcd_wr_data(0x00); lcd_wr_data(0x88); lcd_wr_data(0x00);
    lcd_wr_data(0x77); lcd_wr_data(0x00); lcd_wr_data(0x66); lcd_wr_data(0x00);
    lcd_wr_data(0x88); lcd_wr_data(0x00); lcd_wr_data(0xAA); lcd_wr_data(0x00);
    lcd_wr_data(0xBB); lcd_wr_data(0x00); lcd_wr_data(0x99); lcd_wr_data(0x00);
    lcd_wr_data(0x66); lcd_wr_data(0x00); lcd_wr_data(0x55); lcd_wr_data(0x00);
    lcd_wr_data(0x55); lcd_wr_data(0x00); lcd_wr_data(0x44); lcd_wr_data(0x00);
    lcd_wr_data(0x44); lcd_wr_data(0x00); lcd_wr_data(0x55); lcd_wr_data(0x00);

    lcd_wr_reg(0xE9); lcd_wr_data(0xAA); lcd_wr_data(0x00); lcd_wr_data(0x00); lcd_wr_data(0x00);
    lcd_wr_reg(0x00);  lcd_wr_data(0xAA);
    { uint8_t _i; lcd_wr_reg(0xCF); for (_i=0; _i<16; _i++) lcd_wr_data(0x00); }
    lcd_wr_reg(0xF0); lcd_wr_data(0x00); lcd_wr_data(0x50); lcd_wr_data(0x00);
                       lcd_wr_data(0x00); lcd_wr_data(0x00);
    lcd_wr_reg(0xF3); lcd_wr_data(0x00);
    lcd_wr_reg(0xF9); lcd_wr_data(0x06); lcd_wr_data(0x10); lcd_wr_data(0x29); lcd_wr_data(0x00);

    lcd_wr_reg(0x3A); lcd_wr_data(0x55);   /* 16-bit RGB565 */

    lcd_wr_reg(0x11);                       /* Sleep Out (纯命令, 无数据) */
    lcd_delay_ms(100);

    lcd_wr_reg(0x29);                       /* Display ON (纯命令, 无数据) */

    lcd_wr_reg(0x35); lcd_wr_data(0x00);    /* TE Control */
    lcd_wr_reg(0x51); lcd_wr_data(0xFF);    /* 背光最大 */
    lcd_wr_reg(0x53); lcd_wr_data(0x2C);    /* CTRL Display */
    lcd_wr_reg(0x55); lcd_wr_data(0x82);    /* CABC Control */
    lcd_wr_reg(0x2C);                       /* Memory Write */
}

/* ── 统一初始化入口 ── */
void lcd_init(void)
{
    /* 1. FSMC GPIO + 时序 */
    FSMC_LCD_GPIO_Init();
    FSMC_LCD_Init();

    lcd_delay_ms(50);

    /* 2. 检测 LCD 驱动 IC */
    lcd_id_read();

    /* 3. 发送驱动 IC 初始化序列 */
    switch (lcddev.id) {
    case 0x5310:   /* NT35310 3.5寸 — 竖屏 320x480 */
        LCD_NT35310_InitSeq();
        lcddev.width  = 240;
        lcddev.height = 320;
        break;
    case 0x9341:   /* ILI9341 2.8寸 */
    default:
        LCD_ILI9341_InitSeq();
        lcddev.width  = 240;
        lcddev.height = 320;
        break;
    }

    /* 4. 显示方向: NT35310 竖屏, 其他横屏 */
    lcd_display_dir((lcddev.id == 0x5310) ? 0 : 1);

    /* 5. 背光初始化 & 默认亮度 */
    LCD_BL_Init();
    lcd_bl_set(LCD_BL_DEFAULT);

    /* 6. 清屏 */
    lcd_clear(BLACK);
}

void lcd_display_on(void)
{
    lcd_wr_reg(0x29);            /* Display ON (纯命令, 无参数) */
}

void lcd_display_off(void)
{
    lcd_wr_reg(0x28);            /* Display OFF (纯命令, 无参数) */
}

/* ════════════════════════════════════════════════════════════════
 *  扫描方向 & 显示方向
 * ════════════════════════════════════════════════════════════════ */

void lcd_scan_dir(uint8_t dir)
{
    /* NT35310 不需要 BGR 位; ILI9341 需要 */
    uint16_t bgr = (lcddev.id == 0x9341) ? 0x08 : 0x00;
    uint16_t mem_access;

    switch (dir) {
    case L2R_U2D: mem_access = 0x00; break;
    case L2R_D2U: mem_access = 0x80; break;
    case R2L_U2D: mem_access = 0x40; break;
    case R2L_D2U: mem_access = 0xC0; break;
    case U2D_L2R: mem_access = 0x20; break;
    case U2D_R2L: mem_access = 0x60; break;
    case D2U_L2R: mem_access = 0xA0; break;
    case D2U_R2L: mem_access = 0xE0; break;
    default: mem_access = 0x00; break;
    }

    lcd_write_reg(0x36, mem_access | bgr);
}

void lcd_display_dir(uint8_t dir)
{
    uint16_t tmp;

    lcddev.dir    = dir;
    lcddev.wramcmd = 0x2C;
    lcddev.setxcmd = 0x2A;
    lcddev.setycmd = 0x2B;

    if (dir == 1) {
        /* 横屏: 交换宽高 (宽高已在 lcd_init 按竖屏设置) */
        tmp = lcddev.width;
        lcddev.width  = lcddev.height;
        lcddev.height = tmp;
    }
    /* dir==0 竖屏: 保持 lcd_init 设置的宽高不变 */

    lcd_scan_dir(DFT_SCAN_DIR);
}

/* ════════════════════════════════════════════════════════════════
 *  窗口 & 光标
 * ════════════════════════════════════════════════════════════════ */

/* 设光标 (仅写起始坐标, 不碰结束 — 匹配 ALIENTEK 官方实现) */
void lcd_set_cursor(uint16_t x, uint16_t y)
{
    lcd_wr_reg(lcddev.setxcmd);
    lcd_wr_data(x >> 8);
    lcd_wr_data(x & 0xFF);
    lcd_wr_reg(lcddev.setycmd);
    lcd_wr_data(y >> 8);
    lcd_wr_data(y & 0xFF);
}

void lcd_clear(uint16_t color)
{
    uint32_t total = (uint32_t)lcddev.width * lcddev.height;
    lcd_set_cursor(0, 0);
    lcd_write_ram_prepare();
    while (total--) { LCD->RAM = color; }
}

void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    uint16_t i, j;
    uint16_t xlen = ex - sx + 1;
    for (i = sy; i <= ey; i++) {
        lcd_set_cursor(sx, i);
        lcd_write_ram_prepare();
        for (j = 0; j < xlen; j++) { LCD->RAM = color; }
    }
}

/* ════════════════════════════════════════════════════════════════
 *  绘制函数
 * ════════════════════════════════════════════════════════════════ */

void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
    lcd_set_cursor(x, y);
    lcd_write_ram_prepare();
    LCD->RAM = color;
}

void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    int16_t dx  = (int16_t)(x2 > x1 ? x2 - x1 : x1 - x2);
    int16_t dy  = (int16_t)(y2 > y1 ? y2 - y1 : y1 - y2);
    int16_t sx  = x1 < x2 ? 1 : -1;
    int16_t sy  = y1 < y2 ? 1 : -1;
    int16_t err = dx - dy;
    int16_t e2;

    while (1) {
        lcd_draw_point(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 <  dx) { err += dx; y1 += sy; }
    }
}

void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    lcd_draw_line(x1, y1, x2, y1, color);  /* 上 */
    lcd_draw_line(x1, y1, x1, y2, color);  /* 左 */
    lcd_draw_line(x2, y1, x2, y2, color);  /* 右 */
    lcd_draw_line(x1, y2, x2, y2, color);  /* 下 */
}

void lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 0;

    while (x >= y) {
        lcd_draw_point(x0 + x, y0 + y, color);
        lcd_draw_point(x0 + y, y0 + x, color);
        lcd_draw_point(x0 - y, y0 + x, color);
        lcd_draw_point(x0 - x, y0 + y, color);
        lcd_draw_point(x0 - x, y0 - y, color);
        lcd_draw_point(x0 - y, y0 - x, color);
        lcd_draw_point(x0 + y, y0 - x, color);
        lcd_draw_point(x0 + x, y0 - y, color);

        y++;
        if (err <= 0) {
            err += 2 * y + 1;
        }
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 *  文本渲染 — ASCII 8x16
 *
 *  字库格式: column-major, 每个 byte = 一列 8 个纵向像素 (MSB=顶)
 *            前 8 字节 = 列 0~7 的上半 (row 0~7)
 *            后 8 字节 = 列 0~7 的下半 (row 8~15)
 * ════════════════════════════════════════════════════════════════ */

void lcd_show_char(uint16_t x, uint16_t y, char chr, uint8_t size,
                   uint8_t mode, uint16_t color)
{
    uint8_t  col, row;
    uint8_t  upper, lower;
    const uint8_t *font;

    if (chr < ' ' || chr > '~') return;
    chr = chr - ' ';
    font = (const uint8_t *)asc2_1608;
    font += chr * 16;

    /* 逐列交错格式 — 跟中文一致.
     * 始终用 8x16 字模; 不同 size 只影响 text_show_string 的字间距 */
    (void)size;
    for (col = 0; col < 8; col++) {
        upper = font[col * 2];
        lower = font[col * 2 + 1];
        for (row = 0; row < 8; row++) {
            if (upper & (0x80 >> row))
                lcd_draw_point(x + col, y + row, color);
            else if (mode == 0)
                lcd_draw_point(x + col, y + row, g_back_color);
        }
        for (row = 0; row < 8; row++) {
            if (lower & (0x80 >> row))
                lcd_draw_point(x + col, y + 8 + row, color);
            else if (mode == 0)
                lcd_draw_point(x + col, y + 8 + row, g_back_color);
        }
    }
}

void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                     uint8_t size, const char *p, uint16_t color)
{
    uint16_t x0 = x;
    uint8_t  char_w = 8;  /* 固定 8x16 */

    (void)size;
    if (!p) return;

    while (*p) {
        if (*p == '\n') {
            y += 16;
            x  = x0;
            p++;
            continue;
        }
        if (x + char_w > x0 + width) {
            y += 16;
            x  = x0;
        }
        if (y + 16 > height) break;

        lcd_show_char(x, y, *p, 16, 0, color);
        x += char_w;
        p++;
    }
}

void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len,
                  uint8_t size, uint16_t color)
{
    char buf[12];
    uint8_t i;

    (void)size;
    for (i = 0; i < len && i < 11; i++) {
        buf[len - 1 - i] = (char)('0' + (num % 10));
        num /= 10;
    }
    buf[len] = '\0';

    for (i = 0; i < len; i++) {
        lcd_show_char(x + i * 8, y, buf[i], 16, 0, color);
    }
}

/* ════════════════════════════════════════════════════════════════
 *  文本渲染 — 中文 16x16
 *
 *  字库格式: PCtoLCD2002 "逐列式" C51 格式 (32 字节/字)
 *            byte[col*2]   = 第 col 列上半 (rows 0~7, MSB=row0)
 *            byte[col*2+1] = 第 col 列下半 (rows 8~15, MSB=row8)
 *            前 16 字节 = 列 0~7,  后 16 字节 = 列 8~15
 * ════════════════════════════════════════════════════════════════ */
