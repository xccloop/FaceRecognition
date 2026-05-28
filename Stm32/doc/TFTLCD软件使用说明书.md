# 战舰V4 TFTLCD 软件使用说明书

## 1. 概述

本文档基于 ALIENTEK 战舰V4 开发板（主控 STM32F103ZET6）原理图编写，适用于以下 TFTLCD 模组：

| 模组型号 | 尺寸 | 分辨率 | 驱动IC | 触摸芯片 |
|----------|------|--------|--------|----------|
| ATK-MD0280 | 2.8寸 | 320×240 | ILI9341 | XPT2046 |
| ATK-MD0350 | 3.5寸 | 480×320 | NT35310 | XPT2046 |
| ATK-MD0430 | 4.3寸 | 800×480 | NT35510 | XPT2046 |
| ATK-MD0700 | 7.0寸 | 800×480/1024×600 | SSD1963 | XPT2046 |

## 2. 硬件接口

### 2.1 LCD 接口引脚定义（34-Pin FPC/排针）

TFTLCD 模组通过 34 针接口与主板连接，信号分为三类：**FSMC 并口数据**、**触摸 SPI**、**背光控制**。

### 2.2 FSMC 16位并口（LCD 显示数据通道）

STM32F103ZET6 的 FSMC（灵活的静态存储控制器）Bank1 区域4 驱动 LCD，16 位数据宽度。

| LCD信号 | STM32引脚 | GPIO | FSMC功能 | 说明 |
|---------|-----------|------|----------|------|
| DB0 | 85 | PD14 | FSMC_D0 | 数据位0 |
| DB1 | 86 | PD15 | FSMC_D1 | 数据位1 |
| DB2 | 114 | PD0 | FSMC_D2 | 数据位2 |
| DB3 | 115 | PD1 | FSMC_D3 | 数据位3 |
| DB4 | 58 | PE7 | FSMC_D4 | 数据位4 |
| DB5 | 59 | PE8 | FSMC_D5 | 数据位5 |
| DB6 | 60 | PE9 | FSMC_D6 | 数据位6 |
| DB7 | 63 | PE10 | FSMC_D7 | 数据位7 |
| DB8 | 64 | PE11 | FSMC_D8 | 数据位8 |
| DB9 | 65 | PE12 | FSMC_D9 | 数据位9 |
| DB10 | 66 | PE13 | FSMC_D10 | 数据位10 |
| DB11 | 67 | PE14 | FSMC_D11 | 数据位11 |
| DB12 | 68 | PE15 | FSMC_D12 | 数据位12 |
| DB13 | 77 | PD8 | FSMC_D13 | 数据位13 |
| DB14 | 78 | PD9 | FSMC_D14 | 数据位14 |
| DB15 | 79 | PD10 | FSMC_D15 | 数据位15 |
| LCD_CS | 127 | PG12 | FSMC_NE4 | 片选（Bank1 区域4） |
| LCD_RS | 56 | PG0 | FSMC_A10 | 寄存器/数据选择 |
| LCD_WR | 119 | PD5 | FSMC_NWE | 写使能 |
| LCD_RD | 118 | PD4 | FSMC_NOE | 读使能 |

### 2.3 触摸屏 SPI 接口

触摸控制器 XPT2046 通过 SPI 协议与 MCU 通信（软件模拟 SPI 或硬件 SPI）：

| 信号 | STM32引脚 | GPIO | 说明 |
|------|-----------|------|------|
| T_SCK (T_CLK) | 47 | PB1 | SPI 时钟 |
| T_MISO | 48 | PB2 | SPI 主入从出（复用 BOOT1）|
| T_MOSI | 21 | PF9 | SPI 主出从入 |
| T_CS | 49 | PF11 | SPI 片选 |
| T_PEN | 22 | PF10 | 触摸中断（按下时低电平）|

> **注意**：PB2 同时用作 BOOT1，上电时决定启动模式。作为触摸 MISO 使用时不影响正常运行。

### 2.4 背光控制

| 信号 | STM32引脚 | GPIO | 说明 |
|------|-----------|------|------|
| LCD_BL | 46 | PB0 | PWM 背光控制（定时器输出）|
| BL_VDD | - | 5V/VCC | 背光供电（通过模组板升压）|

## 3. 软件驱动架构

```
main.c
  ├─ lcd_init()          // LCD 初始化
  │   ├─ FSMC_LCD_Init()  // FSMC 配置
  │   └─ LCD_Init_Seq()   // 发送初始化序列（不同驱动IC不同）
  ├─ lcd_clear(color)     // 清屏
  ├─ lcd_draw_point(x,y)  // 画点
  ├─ lcd_show_string()    // 显示字符串
  └─ touch_scan()         // 触摸扫描（循环调用）
```

## 4. FSMC 配置（关键代码）

### 4.1 GPIO 初始化

```c
// FSMC 全部数据线和控制线 GPIO 配置
void FSMC_LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE |
                           RCC_APB2Periph_GPIOF | RCC_APB2Periph_GPIOG, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC, ENABLE);

    // 所有 FSMC 引脚配置为复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;

    // PD: FSMC_D0~D3, D13~D15, FSMC_NOE, FSMC_NWE
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_4 | GPIO_Pin_5 |
                                  GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 |
                                  GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    // PE: FSMC_D4~D12
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 |
                                  GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 |
                                  GPIO_Pin_15;
    GPIO_Init(GPIOE, &GPIO_InitStructure);

    // PG: FSMC_NE4 (LCD_CS), FSMC_A10 (LCD_RS)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_12;
    GPIO_Init(GPIOE, &GPIO_InitStructure);
}
```

### 4.2 FSMC 时序配置

```c
void FSMC_LCD_Init(void)
{
    FSMC_NORSRAMInitTypeDef  FSMC_NORSRAMInitStructure;
    FSMC_NORSRAMTimingInitTypeDef  p;

    // 地址建立时间 (ADDSET)
    p.FSMC_AddressSetupTime = 0x02;   // 2 个 HCLK
    // 地址保持时间 (ADDHLD)
    p.FSMC_AddressHoldTime  = 0x00;   // 0 个 HCLK
    // 数据建立时间 (DATAST)
    p.FSMC_DataSetupTime    = 0x05;   // 5 个 HCLK
    // 总线周转时间
    p.FSMC_BusTurnAroundDuration = 0x00;
    // 时钟分频
    p.FSMC_CLKDivision = 0x00;
    // 数据保持时间
    p.FSMC_DataLatency = 0x00;
    // 访问模式：A 模式（OE 在整个访问期间有效）
    p.FSMC_AccessMode = FSMC_AccessMode_A;

    FSMC_NORSRAMInitStructure.FSMC_Bank                  = FSMC_Bank1_NORSRAM4;
    FSMC_NORSRAMInitStructure.FSMC_DataAddressMux        = FSMC_DataAddressMux_Disable;
    FSMC_NORSRAMInitStructure.FSMC_MemoryType            = FSMC_MemoryType_SRAM;
    FSMC_NORSRAMInitStructure.FSMC_MemoryDataWidth       = FSMC_MemoryDataWidth_16b;
    FSMC_NORSRAMInitStructure.FSMC_BurstAccessMode       = FSMC_BurstAccessMode_Disable;
    FSMC_NORSRAMInitStructure.FSMC_AsynchronousWait      = FSMC_AsynchronousWait_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WaitSignalPolarity     = FSMC_WaitSignalPolarity_Low;
    FSMC_NORSRAMInitStructure.FSMC_WrapMode              = FSMC_WrapMode_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WaitSignalActive      = FSMC_WaitSignalActive_BeforeWaitState;
    FSMC_NORSRAMInitStructure.FSMC_WriteOperation         = FSMC_WriteOperation_Enable;
    FSMC_NORSRAMInitStructure.FSMC_WaitSignal             = FSMC_WaitSignal_Disable;
    FSMC_NORSRAMInitStructure.FSMC_ExtendedMode           = FSMC_ExtendedMode_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WriteBurst             = FSMC_WriteBurst_Disable;
    FSMC_NORSRAMInitStructure.FSMC_ReadWriteTimingStruct  = &p;
    FSMC_NORSRAMInitStructure.FSMC_WriteTimingStruct      = &p;

    FSMC_NORSRAMInit(&FSMC_NORSRAMInitStructure);
    FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM4, ENABLE);
}
```

### 4.3 LCD 基地址定义

使用 FSMC_NE4 (Bank1 区域4)，起始地址 `0x6C000000`。
RS 信号连接 FSMC_A10，对应地址偏移 `(1 << 10) = 0x400`。

```c
// LCD 基地址 (Bank1, Region4, 0x6C000000)
#define LCD_BASE        ((uint32_t)(0x60000000 | 0x0C000000))

// RS=0: 命令寄存器; RS=1: 数据寄存器 (FSMC_A10)
#define LCD_REG         (*(__IO uint16_t *)(LCD_BASE))
#define LCD_RAM         (*(__IO uint16_t *)(LCD_BASE + (1 << (10 + 1))))

// 注：16位数据宽度时，地址左移1位，实际偏移 = 1 << (10+1) = 0x800
```

## 5. LCD 驱动函数

### 5.1 基础读写操作

```c
// 写命令
static inline void LCD_WR_REG(uint16_t reg)
{
    LCD_REG = reg;
}

// 写数据
static inline void LCD_WR_DATA(uint16_t data)
{
    LCD_RAM = data;
}

// 读数据
static inline uint16_t LCD_RD_DATA(void)
{
    return LCD_RAM;
}

// 写寄存器（先发寄存器地址，再发数据）
void LCD_WriteReg(uint16_t reg, uint16_t value)
{
    LCD_REG = reg;
    LCD_RAM = value;
}

// 读寄存器
uint16_t LCD_ReadReg(uint16_t reg)
{
    LCD_REG = reg;
    return LCD_RAM;
}
```

### 5.2 初始化序列（以 ILI9341 为例）

```c
void LCD_ILI9341_Init(void)
{
    // 硬件复位
    LCD_RST_LOW();
    delay_ms(20);
    LCD_RST_HIGH();
    delay_ms(20);

    // 退出休眠
    LCD_WriteReg(0x01, 0x0000);   // Software Reset
    delay_ms(120);

    LCD_WriteReg(0x11, 0x0000);   // Sleep Out
    delay_ms(120);

    // 像素格式: 16bit MCU 接口
    LCD_WriteReg(0x3A, 0x0055);

    // 帧速率控制
    LCD_WriteReg(0xB1, 0x001B);   // Frame Rate Control
    LCD_WriteReg(0xB6, 0x000A);   // Display Function Control
    LCD_WriteReg(0xC0, 0x000F);   // Power Control 1
    LCD_WriteReg(0xC1, 0x0000);   // Power Control 2
    LCD_WriteReg(0xC5, 0x0030);   // VCOM Control 1
    LCD_WriteReg(0xC7, 0x00C8);   // VCOM Control 2

    // 内存访问控制: 从左到右，从上到下
    LCD_WriteReg(0x36, 0x0008);

    // 显示方向
    LCD_WriteReg(0x2A, 0x0000);   // Column Start
    LCD_WriteReg(0x2B, 0x0000);   // Row Start

    LCD_WriteReg(0x29, 0x0000);   // Display ON
}
```

### 5.3 窗口设置

```c
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_WriteReg(0x2A, x1 >> 8);   // 列起始地址高8位
    LCD_WriteReg(0x2B, x1 & 0xFF); // 列起始地址低8位
    LCD_WriteReg(0x2C, x2 >> 8);   // 列结束地址高8位
    LCD_WriteReg(0x2D, x2 & 0xFF); // 列结束地址低8位

    LCD_WriteReg(0x2E, y1 >> 8);   // 行起始地址高8位
    LCD_WriteReg(0x2F, y1 & 0xFF); // 行起始地址低8位
    LCD_WriteReg(0x30, y2 >> 8);   // 行结束地址高8位
    LCD_WriteReg(0x31, y2 & 0xFF); // 行结束地址低8位

    LCD_WriteReg(0x2C, 0x0000);    // 准备写GRAM
}
```

### 5.4 画点函数

```c
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_SetWindow(x, y, x, y);
    LCD_WR_DATA(color);
}

uint16_t LCD_ReadPoint(uint16_t x, uint16_t y)
{
    LCD_SetWindow(x, y, x, y);
    LCD_WriteReg(0x2E, 0x0000); // 读GRAM命令
    return LCD_RD_DATA();
}
```

### 5.5 区域填充

```c
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint32_t pixels = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);

    LCD_SetWindow(x1, y1, x2, y2);

    while (pixels--) {
        LCD_WR_DATA(color);
    }
}
```

### 5.6 清屏

```c
void LCD_Clear(uint16_t color)
{
    LCD_Fill(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color);
}
```

## 6. 触摸屏驱动

### 6.1 XPT2046 初始化

```c
// GPIO 初始化
void Touch_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOF, ENABLE);

    // T_SCK (PB1), T_MOSI (PF9), T_CS (PF11) - 推挽输出
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_11;
    GPIO_Init(GPIOF, &GPIO_InitStructure);

    // T_MISO (PB2) - 浮空输入
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // T_PEN (PF10) - 上拉输入（中断引脚）
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOF, &GPIO_InitStructure);
}
```

### 6.2 SPI 读写

XPT2046 使用 SPI Mode 0 (CPOL=0, CPHA=0)，12位ADC数据。

```c
// 软件模拟SPI读写一个字节
uint8_t Touch_SPI_RW(uint8_t data)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        T_CLK_LOW();
        if (data & 0x80) T_MOSI_HIGH();
        else             T_MOSI_LOW();
        data <<= 1;
        T_CLK_HIGH();
        if (T_MISO_READ()) data |= 1;
    }
    T_CLK_LOW();
    return data;
}

// 读取XPT2046 ADC值（通道选择）
// cmd: 0x90=X, 0xD0=Y
uint16_t Touch_ReadADC(uint8_t cmd)
{
    uint16_t value;

    T_CS_LOW();
    Touch_SPI_RW(cmd);
    value  = Touch_SPI_RW(0x00) << 8;
    value |= Touch_SPI_RW(0x00);
    value >>= 3;    // 12位有效，右移3位
    T_CS_HIGH();

    return value;
}
```

### 6.3 触摸坐标读取

```c
// 读触摸坐标（带滤波）
uint8_t Touch_Scan(uint16_t *x, uint16_t *y)
{
    uint16_t x_raw, y_raw;

    // 检查触摸中断：低电平表示触摸按下
    if (T_PEN_READ() == 1) {
        return 0;   // 无触摸
    }

    // 多次采样取平均值（消抖）
    x_raw = Touch_ReadADC(0x90);   // X通道
    y_raw = Touch_ReadADC(0xD0);   // Y通道

    // 坐标转换（根据液晶分辨率校准）
    *x = (x_raw * LCD_WIDTH)  / 4096;
    *y = (y_raw * LCD_HEIGHT) / 4096;

    return 1;   // 有触摸
}
```

## 7. 背光控制（PWM）

```c
void LCD_BL_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_OCInitTypeDef TIM_OCInitStructure;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);

    // PB0 → TIM3_CH3 输出 PWM
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // 部分重映射：TIM3_CH3 映射到 PB0
    GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3, ENABLE);

    // TIM3 配置: 1kHz PWM, 占空比0~100%
    TIM_TimeBaseStructure.TIM_Period = 899;    // ARR: 900-1
    TIM_TimeBaseStructure.TIM_Prescaler = 0;
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStructure);

    // PWM 模式1: 占空比 = CCR/ARR
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = 0;         // 初始占空比=0
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC3Init(TIM3, &TIM_OCInitStructure);

    TIM_Cmd(TIM3, ENABLE);
}

// 设置背光亮度 (0~100)
void LCD_BL_Set(uint8_t brightness)
{
    uint16_t pwm_val;

    if (brightness > 100) brightness = 100;
    pwm_val = (uint16_t)(brightness * 899 / 100);

    TIM_SetCompare3(TIM3, pwm_val);
}
```

## 8. 完整使用示例

```c
#include "lcd.h"
#include "touch.h"
#include "delay.h"

int main(void)
{
    uint16_t touch_x, touch_y;
    uint16_t color = RED;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    delay_init(72);   // 系统时钟72MHz

    // 1. 初始化 LCD
    LCD_Init();            // ILI9341 (2.8寸) 初始化
    LCD_BL_Set(80);        // 背光亮度 80%

    // 2. 清屏为白色
    LCD_Clear(WHITE);

    // 3. 显示文字
    LCD_ShowString(10, 10, 200, 16, 16, "TFTLCD Test");
    LCD_ShowString(10, 30, 200, 16, 16, "ALIENTEK WarShip V4");

    // 4. 画矩形
    LCD_DrawRectangle(50, 50, 200, 150, BLUE);

    // 5. 触摸循环
    while (1) {
        if (Touch_Scan(&touch_x, &touch_y)) {
            // 画触摸点
            LCD_DrawPoint(touch_x, touch_y, color);
        }
    }
}
```

## 9. 不同驱动IC适配说明

| LCD模组 | 驱动IC | LCD_Init调用 | 分辨率宏 |
|---------|--------|-------------|----------|
| 2.8寸 | ILI9341 | `LCD_ILI9341_Init()` | 240×320 |
| 3.5寸 | NT35310 | `LCD_NT35310_Init()` | 320×480 |
| 4.3寸 | NT35510 | `LCD_NT35510_Init()` | 480×800 |
| 7寸 | SSD1963 | `LCD_SSD1963_Init()` | 800×480 |

各模组的 FSMC 硬件接口完全兼容（34-Pin 统一接口），更换不同尺寸屏幕只需调用对应驱动 IC 的初始化函数，无需修改硬件连接。

## 10. 常见问题与注意事项

**Q: 屏幕无显示（全白或全黑）？**
1. 检查 FSMC GPIO 初始化是否正确（所有 D0~D15、RS、WR、RD、CS）
2. 确认 LCD_RST 引脚复位时序（至少 20ms 低电平复位）
3. 检查背光 BL 是否开启（PB0 PWM 输出）

**Q: 触摸无反应？**
1. 检查 `T_PEN` (PF10) 的中断/电平检测
2. 确认触摸 SPI 时序（CPOL=0, CPHA=0）
3. PB2 (T_MISO) 需避开 BOOT1 配置冲突，上电后正常

**Q: 不同尺寸屏幕如何切换？**
- 替换 `lcd_init()` 中的驱动IC初始化函数
- 更新 `LCD_WIDTH` / `LCD_HEIGHT` 分辨率宏
- 触摸坐标映射公式自动适配（由分辨率决定）

**Q: FSMC 地址计算？**
- FSMC_NE4 映射到地址 0x6C000000
- RS=FSMC_A10：命令地址 0x6C000000，数据地址 0x6C000800（16位宽偏移）
- 16位数据宽度时 HADDR[25:1] → FSMC_A[24:0]，即地址线左移1位

## 11. 引脚占用速查表

```
┌─────────────────────────────────────────────┐
│ TFTLCD 接口引脚占用一览                       │
├──────────┬────────┬──────────────────────────┤
│ GPIO     │ 引脚   │ 功能                     │
├──────────┼────────┼──────────────────────────┤
│ PB0      │ 46     │ LCD_BL (背光PWM)         │
│ PB1      │ 47     │ T_SCK (触摸时钟)         │
│ PB2      │ 48     │ T_MISO (触摸数据入)      │
│ PF9      │ 21     │ T_MOSI (触摸数据出)      │
│ PF10     │ 22     │ T_PEN (触摸中断)         │
│ PF11     │ 49     │ T_CS (触摸片选)          │
│ PD14     │ 85     │ FSMC_D0                  │
│ PD15     │ 86     │ FSMC_D1                  │
│ PD0      │ 114    │ FSMC_D2                  │
│ PD1      │ 115    │ FSMC_D3                  │
│ PE7~PE15 │ 58~68  │ FSMC_D4~D12              │
│ PD8~PD10 │ 77~79  │ FSMC_D13~D15             │
│ PG12     │ 127    │ FSMC_NE4 (LCD_CS)        │
│ PG0      │ 56     │ FSMC_A10 (LCD_RS)        │
│ PD5      │ 119    │ FSMC_NWE (LCD_WR)        │
│ PD4      │ 118    │ FSMC_NOE (LCD_RD)        │
└──────────┴────────┴──────────────────────────┘

安全等级说明（来自原理图引脚分配表）：
  Y = 完全占用：使用TFTLCD功能时不可复用
  N = 部分占用：关闭LCD片选后可作普通IO
```
