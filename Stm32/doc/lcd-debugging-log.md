# TFTLCD 调试记录 — 从白屏到全功能显示

> 硬件: 战舰V4 (STM32F103ZET6) + 3.5寸 NT35310 TFTLCD (240×320 竖屏)  
> 参考: ALIENTEK HAL 例程 → SPL 移植  
> **注**: 初期误设为 320×480 导致部分调试走了弯路, 最终确认实际分辨率为 240×320

---

## 问题清单

### 1. FSMC 基地址偏移缺失

**现象**: 上电白屏, 无任何显示

**原因**: `LCD_BASE = 0x6C000000`, struct 偏移 2 字节不够跨越 FSMC_A10 地址边界。  
`LCD->REG` (offset 0) 和 `LCD->RAM` (offset 2) 都落在 RS=0 (命令模式), 像素数据被当成命令发送, GRAM 从未写入。

**修复**: 加 `((1 << 10) * 2 - 2) = 0x7FE` 偏移:
```c
#define LCD_BASE  ((0x60000000 + 0x0C000000) | (((1 << 10) * 2) - 2))
// REG=0x6C0007FE (RS=0), RAM=0x6C000800 (RS=1)
```

---

### 2. TIM3 PartialRemap 引脚冲突

**现象**: LED 亮则屏幕灭, LED 灭则屏幕亮, 二者互斥

**原因**: `GPIO_PinRemapConfig(GPIO_PartialRemap_TIM3)` 把 TIM3_CH2 映射到 PB5, 与 LED_ERR 共用引脚。TIM3 启动后 PB5 被硬件接管。

**修复**: 去掉 PartialRemap。TIM3_CH3 默认就在 PB0, 不需要重映射。

---

### 3. NT35310 vs ILI9341 驱动 IC 不同

**现象**: 黑底能铺满, 但文字渲染错乱, 字符上下半颠倒

**原因**: 代码默认 ILI9341 (2.8寸) 初始化序列, 但实际是 NT35310 (3.5寸)。两 IC 的寄存器地址、伽马表、初始化流程完全不同。

**修复**: 
- 从 ALIENTEK HAL 例程提取完整 NT35310 初始化序列 (~200+ 寄存器写入)
- 加多 IC 自动检测: 先试 0xD3 (ILI9341), 再试 0xD4 (NT35310)
- 竖屏分辨率设为 320×480 (NT35310 GRAM 原生尺寸)

---

### 4. lcd_show_char 高度检查 bug

**现象**: 5 个单点能显示, 但所有 ASCII 文字不出现

**原因**: `lcd_show_string` 第 4 参数 `height` 是最大 Y 边界, 但调用处传的是字体大小 16。  
`y=120, y+16=136, height=16 → 136>16 → break`, 一个字都不画。

**修复**: 调用处 `height` 改为 `LCD_HEIGHT` (实际屏幕高度)。

---

### 5. ASCII 字库数据全错

**现象**: 文字能显示但方向/上下颠倒, "TestOK" 中只有 OK 正常

**原因**: 手打的 `asc2_1608[95][16]` 字体数据完全错误, 格式不匹配。

**修复**: 从 ALIENTEK `lcdfont.h` 提取正确的 ASCII 字库数据 (95 字符 × 16 字节, 逐列交错格式)。

---

### 6. 字体渲染格式错配

**现象**: 替换正确字库后反而完全乱码

**原因**: 参考代码的 ASCII 字库是**逐列交错格式** (`font[col*2]=上半, font[col*2+1]=下半`), 但渲染代码按**分组格式** (`font[0..7]=上半, font[8..15]=下半`) 读取。

**修复**: 改为跟中文一致的逐列式渲染:
```c
for (col = 0; col < 8; col++) {
    upper = font[col * 2];
    lower = font[col * 2 + 1];
    ...
}
```

---

### 7. lcd_set_cursor 窗口缩小问题

**现象**: 切换状态后上一个状态的文字残留

**原因**: `lcd_set_cursor(x, y)` 设置起始坐标后, 批量写像素时自动递增。但先前文字渲染把结束坐标缩到了很小的位置。后续 `lcd_clear` 只设起始坐标, 结束坐标仍是缩小后的值, 清屏只清了 1 像素区域。

**修复**: `lcd_set_cursor` 改为只写起始坐标 (2 参数), 永远不碰结束坐标。结束坐标保持 NT35310 默认全屏值。  
此做法匹配 ALIENTEK 官方实现。

---

### 8. lcd_clear 显式设结束坐标导致底部白屏

**现象**: 底部有一段白色无法覆盖

**原因**: 尝试在 `lcd_clear` 中显式设全屏窗口 (起始+结束), 但 NT35310 对显式设置的结束坐标响应异常。只设起始坐标 (依赖默认结束值) 反而正确。

**修复**: `lcd_clear` / `lcd_fill` 只设起始坐标, 不设结束。配合 `lcd_set_cursor` 的 2 参数方案一致。

---

### 9. SPI Flash 初始化顺序

**现象**: LCD 汉字不显示, 只有 "FaceRec" 残影在右上角

**原因**: `display_init()` → `display_show_boot()` → 读 Flash 字库的汉字 → `norflash_read()` 需要 SPI2 已初始化。但 `norflash_init()` 在 `display_init()` 之后才调用。

**修复**: 调换顺序: 先 `norflash_init()` + `fonts_init()`, 再 `display_init()`。

---

### 10. GBK 编码 vs UTF-8

**现象**: Keil 编译报错 `invalid multibyte character sequence`, 中文字符串解析失败

**原因**: Keil ARMCC v5 不支持 UTF-8 中文源码。display.c 中直接写的 "识别成功" 等字符串被当成非法多字节序列。

**修复**: 全部中文字符串改为 `\xHH` 转义序列 (GBK 编码):
```c
static const char IDENTIFY_OK[] = {"\xCA\xB6\xB1\xF0\xB3\xC9\xB9\xA6"};  // 识别成功
```

---

## 关键设计原则

1. **lcd_set_cursor 只设起始, 不设结束**: NT35310 不能接受显式设置的结束坐标。
2. **批量操作直接用寄存器, 不走 lcd_set_window**: 避开结束坐标问题。
3. **SPI Flash 受限于 SPI2 总线**: PB13-15 与 FSMC 不冲突 (FSMC 数据线在 PD/PE, 地址线在 PG)。
4. **字库用 GBK 编码**: 全字库 216KB 存在 W25Q128, 首次从 SD 卡写入后永久可用。
5. **串口静默**: 启动后只输出横幅, 运行时零诊断输出, 避免干扰帧协议。
