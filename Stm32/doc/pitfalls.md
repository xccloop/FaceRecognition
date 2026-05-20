# STM32 开发踩坑记录

> 记录本项目 STM32 端开发过程中遇到的实际问题和解决方案，方便后续维护时快速排查。

## 目录

1. [FreeRTOS 相关](#1-freertos-相关)
2. [UART / 帧协议相关](#2-uart--帧协议相关)
3. [硬件外设相关](#3-硬件外设相关)
4. [Keil 开发环境相关](#4-keil-开发环境相关)

---

## 1. FreeRTOS 相关

### 1.1 ISR 中调用非 FromISR 版本的 API → HardFault

**现象：** 在 `USART2_IRQHandler` 中调用 `xQueueSend()`（非 ISR 版本），程序直接 HardFault。

**原因：** FreeRTOS 的 ISR 版本 API（`xQueueSendFromISR` 等）与普通版本内部逻辑不同。ISR 版本不会阻塞（ISR 中不能阻塞），且会通过 `pxHigherPriorityTaskWoken` 参数告知调度器是否需要上下文切换。

**解决方案：**

```c
// ❌ 错误：ISR 中调用普通 API
void USART2_IRQHandler(void) {
    xQueueSend(xUartRxQueue, &ch, 0);  // HardFault!
}

// ✓ 正确：使用 FromISR 版本
void USART2_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(xUartRxQueue, &ch, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);  // 如果需要切换，触发 PendSV
}
```

### 1.2 NVIC 优先级分组配置顺序错误

**现象：** 调用 `xTaskCreate` 或其他 FreeRTOS API 时报 `configASSERT` 失败或行为异常。

**原因：** FreeRTOS 要求 `NVIC_PriorityGroupConfig()` 必须在任何 FreeRTOS API 调用之前执行。FreeRTOS 使用优先级分组 4（4 位全部分给抢占优先级，0 位给子优先级）。

**解决方案：** `main()` 中第一行就调用：

```c
int main(void) {
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);  // ★ 必须在最前面
    // ... 后续初始化
}
```

### 1.3 任务栈溢出 — 程序随机卡死或重启

**现象：** 程序运行一段时间后莫名其妙卡死，或者触发 `vApplicationStackOverflowHook`。

**原因：** 任务栈太小，或者函数中定义了太大的局部变量（尤其是大数组）。

**排查方法：**

```c
// 在任务中加：
UBaseType_t remaining = uxTaskGetStackHighWaterMark(NULL);
// 如果 remaining 很小（比如 < 20），说明栈快爆了
```

**解决方案：**
1. 增大 `configMINIMAL_STACK_SIZE`（当前 128 字 = 512 字节）
2. 避免在任务函数中声明大数组，改用静态变量或堆分配
3. 如果某个特定任务需要更大栈，创建时单独指定：

```c
xTaskCreate(vMyTask, "MyTask", 256, NULL, 3, &h);  // 256 字栈
```

### 1.4 静态内存分配未提供钩子 → 编译失败

**现象：** 编译时报错 `undefined reference to vApplicationGetIdleTaskMemory` 等。

**原因：** `FreeRTOSConfig.h` 中 `configSUPPORT_STATIC_ALLOCATION = 1` 后，FreeRTOS 要求用户提供空闲任务和定时器任务的内存。

**解决方案：** 在 main.c 中实现以下钩子：

```c
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

// 同理 vApplicationGetTimerTaskMemory()
```

### 1.5 ISR 优先级高于 configMAX_SYSCALL_INTERRUPT_PRIORITY → 潜在的竞态问题

**现象：** 偶尔出现队列数据错乱或调度异常。

**原因：** 如果 ISR 的逻辑优先级高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY`（即数值上 < 191），ISR 会抢占 FreeRTOS 内核的临界区，导致数据竞争。

**注意：STM32 Cortex-M3 的优先级是数值越大 = 逻辑优先级越低。**

```
数值 0  → 最高逻辑优先级
数值 15 → 最低逻辑优先级

configMAX_SYSCALL_INTERRUPT_PRIORITY = 191 (数值)
对应逻辑优先级 = 191 >> 4 = 11

ISR 逻辑优先级 0~11  → 不可调用 FreeRTOS API
ISR 逻辑优先级 12~15 → 可以调用 FreeRTOS API
```

**解决方案：**

```c
// USART2 中断优先级设为 13（数值 208）
NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 13;  // 13 >= 11 ✓
```

## 2. UART / 帧协议相关

### 2.1 USART ORE（过载错误）导致接收死锁

**现象：** 程序运行一段时间后，`USART_IT_RXNE` 不再触发，UART 接收彻底卡死。

**原因：** 当接收缓冲区（DR 寄存器）中的数据未被及时读取，新数据又到达时，会触发 ORE（Overrun Error）。ORE 置位后，RXNE 不再置位，接收永久中断。

**解决方案：** 在 ISR 中主动检查并清除 ORE：

```c
void USART2_IRQHandler(void) {
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) {
        uint8_t ch = (uint8_t)USART_ReceiveData(USART2);
        xQueueSendFromISR(xUartRxQueue, &ch, &woken);
    }

    // ★ 关键：检查并清除 ORE
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET) {
        (void)USART_ReceiveData(USART2);  // 读 DR 清除 ORE
    }

    portYIELD_FROM_ISR(woken);
}
```

### 2.2 帧解析中的"伪帧头"问题

**现象：** 数据区中出现 0xAA 字节时，接收端误判为新帧的开始，导致丢帧。

**原因：** 初版协议没有转义机制，数据区中出现 0xAA 就会被当成帧头。

**解决方案：** 引入字节转义协议：

```
发送侧编码：
  原始 0xAA → 0xBB 0x55
  原始 0x55 → 0xBB 0xAA
  原始 0xBB → 0xBB 0x44

接收侧解码：
  遇到 0xBB → 进入转义状态，等待下一字节
    0xBB 0x55 → 输出 0xAA
    0xBB 0xAA → 输出 0x55
    0xBB 0x44 → 输出 0xBB
    其他组合 → 非法帧，丢弃
```

**实现要点：** 转义只对 DATA 区进行，帧头 0xAA、帧尾 0x55、CMD、DIR、LEN、XOR 等字段不转义。

### 2.3 帧解析状态机"一步错步步错"

**现象：** 一旦某帧的某个字节出错（如校验失败），后续的帧也无法正确解析。

**原因：** 状态机在校验失败后没有重置到 `STATE_SYNC`，继续在错误状态下读下一个字节。

**解决方案：** 任何状态出错（校验失败、数据长度超限、帧尾不匹配等）立即：

```c
state    = STATE_SYNC;  // 回到搜帧头
esc_state = 0;           // 重置转义状态
```

### 2.4 XOR 校验覆盖范围错误

**现象：** 接收端校验通过，但数据实际有误（或反之，明明是正确数据却校验失败）。

**原因：** XOR 校验的覆盖范围不一致。发送端算的字段集合和接收端算的不一样。

**规范定义：**

```
发送端 XOR = CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
接收端 XOR = 同上

注意：帧头 0xAA 和帧尾 0x55 不参与 XOR 校验！
```

### 2.5 UART 发送时未等待 TXE → 数据丢失

**现象：** 连续发送多个字节时，只有第一个字节被正确发送。

**原因：** 在 `USART_SendData()` 之后没有等待 `USART_FLAG_TXE` 就立即发送下一个字节，覆盖了还未发完的数据。

**解决方案：**

```c
void uart_send_byte(uint8_t b) {
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);  // ★ 必须等
    USART_SendData(USART2, b);
}
```

## 3. 硬件外设相关

### 3.1 IWDG 初始化后不喂狗 → 持续复位

**现象：** 使能 IWDG 后，程序周期性复位（约 2 秒一次）。

**原因：** `IWDG_Enable()` 之后看门狗立即开始倒计时。如果不在 2 秒内喂狗，系统复位。FreeRTOS 调度器启动前的初始化阶段耗时可超过 2 秒。

**解决方案：**
1. 在 `vTaskStartScheduler()` 之前先喂一次狗
2. 启动喂狗定时器后再启动调度器
3. 如果初始化阶段实在太长，增大重载值：

```c
IWDG_SetReload(2500);  // 2500 / 625 = 4 秒超时
```

### 3.2 IWDG 时钟 LSI 未就绪 → HardFault

**现象：** 调用 `IWDG_SetPrescaler()` 或 `IWDG_Enable()` 时 HardFault。

**原因：** IWDG 使用 LSI（内部低速时钟，40kHz）。如果 LSI 未被使能或未就绪，访问 IWDG 寄存器会触发总线错误。

**解决方案：**

```c
static void IWDG_Init(void) {
    RCC_LSICmd(ENABLE);                           // 显式使能 LSI
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);  // 等待就绪

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(1250);
    IWDG_ReloadCounter();
    IWDG_Enable();
}
```

### 3.3 GPIO 时钟未使能 → 引脚无反应

**现象：** `GPIO_SetBits(GPIOA, GPIO_Pin_0)` 执行了但 PA0 电平不变。

**原因：** 没有使能 GPIOA 的时钟。STM32 所有外设（包括 GPIO）都需要先开时钟。

**解决方案：**

```c
RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);  // ★ 必须先开时钟
// 然后再配置 GPIO 模式
GPIO_Init(GPIOA, &GPIO_InitStructure);
```

**注意：** 如果多个外设共用同一总线时钟（如 PA0 和 USART2 都挂在 APB2），只需使能一次即可。但如果一个初始化函数里开了时钟另一个没开，要注意调用顺序。

## 4. Keil 开发环境相关

### 4.1 编译通过但烧录后不运行

**现象：** Keil 编译 0 Error 0 Warning，烧录成功，但程序不运行或行为异常。

**常见原因与解决：**

1. **启动文件不匹配**：`startup_stm32f10x_md.s`（MD=中容量，128KB Flash） vs `startup_stm32f10x_ld.s`（LD=小容量）。STM32F103C8T6 应使用 `_md` 版本。

2. **堆栈大小不足**：在启动文件 `.s` 中：
   ```asm
   Stack_Size  EQU  0x00000400  ; 1024 字节
   Heap_Size   EQU  0x00000200  ; 512 字节
   ```

3. **优化等级问题**：Keil → Options → C/C++ → Optimization 设为 `-O0`（调试阶段）或 `-O2`（发布阶段）。过高优化可能消除关键代码。

### 4.2 USE_FULL_ASSERT 导致编译失败

**现象：** 取消注释 `#define USE_FULL_ASSERT 1` 后编译报错 `undefined reference to assert_failed`。

**原因：** FWlib 中的 `assert_param()` 宏在 `USE_FULL_ASSERT` 打开时会调用 `assert_failed()`，但用户没有实现。

**解决方案：** 在 main.c 中提供实现：

```c
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
    // 可以在这里设断点
    for (;;) {}
}
#endif
```

### 4.3 FreeRTOS 源文件未加入工程 → 链接失败

**现象：** 编译报大量 `undefined reference to xQueueCreate`、`vTaskDelay` 等。

**原因：** FreeRTOS 的 `.c` 源文件没有被加入 Keil 工程。

**解决方案：** 在 Keil 工程中右键 → Add Existing Files，添加：
- `FreeRTOS/tasks.c`
- `FreeRTOS/queue.c`
- `FreeRTOS/timers.c`
- `FreeRTOS/list.c`
- `FreeRTOS/event_groups.c`
- `FreeRTOS/heap_4.c`
- `FreeRTOS/portable/RVDS/ARM_CM3/port.c`

### 4.4 编译警告 "unused parameter" 如何处理

**现象：** 回调函数参数未使用，Keil 报 warning。

**标准做法：**

```c
void vHeartbeatTimerCallback(TimerHandle_t xTimer) {
    (void)xTimer;  // 显式标记参数未使用
    // ...
}
```

不推荐禁掉 "unused parameter" 警告——它有时能帮你发现真正的 bug。

---

> **总结：** 大部分问题出在三个地方：① FreeRTOS ISR API 使用不当（忘记 FromISR）；② 外设时钟未使能；③ 状态机未正确处理错误状态。掌握这三个模式后，大部分 bug 都能快速定位。
