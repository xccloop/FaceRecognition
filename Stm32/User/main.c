#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "uart_dma.h"
#include "frame_protocol.h"
#include "cmd_handler.h"
#include "../BSP/LCD/display.h"
#include "../BSP/NORFLASH/norflash.h"
#include "../Middlewares/TEXT/fonts.h"
#include "../BSP/LED/led.h"

/* -------- 全局句柄 -------- */
static TaskHandle_t  xUartRxTaskHandle = NULL;
static TimerHandle_t xHeartbeatTimer   = NULL;
static TimerHandle_t xFrameTimeoutTimer = NULL;
static uart_dma_handle_t *hUart = NULL;

/* DMA ringbuffer，大小必须是 2 的幂 */
static uint8_t uart_tx_rb_buf[1024];
static uint8_t uart_rx_rb_buf[1024];

/* 帧接收时间戳，cmd_handler 更新，定时器检查 */
TickType_t xLastFrameTicks = 0;
/* 诊断输出防重复标志（cmd_handler 共享） */
uint8_t serial_heartbeat_timeout_sent = 0;
uint8_t serial_heartbeat_restored     = 0;
uint8_t serial_has_received_frame     = 0;  /* 是否曾收到过 Pi 的帧 */


/* -------- 调试文本输出（裸字符串，不走帧协议） -------- */
static void uart_send_text(const char *msg)
{
    uint16_t len = 0;
    while (msg[len]) len++;
    if (len && hUart) uart_dma_send(hUart, (uint8_t *)msg, len);
}

/* cmd_handler.c 调用此函数输出诊断 */
void uart_send_text_line(const char *msg)
{
    uart_send_text(msg);
    uart_send_text("\r\n");
}

/* cmd_handler.c 调用此函数发送二进制帧 */
void uart_send_frame(uint8_t cmd, uint8_t dir,
                     const uint8_t *data, uint16_t data_len)
{
    uint8_t  buf[MAX_COBS_LEN];
    uint16_t total = frame_encode(cmd, dir, data, data_len, buf, sizeof(buf));
    if (total && hUart) uart_dma_send(hUart, buf, total);
}

/* -------- 心跳超时检查（1s 定时器） -------- */
static void vHeartbeatTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;

    /* 启动闪烁: 调度器启动后第 1 个 tick 执行 */
    {
        static uint8_t boot_done = 0;
        if (!boot_done) { boot_done = 1; led_boot_flash(); }
    }

    /* 超时 LED 闪烁驱动 */
    led_timeout_tick();

    if ((xTaskGetTickCount() - xLastFrameTicks) * portTICK_PERIOD_MS > 60000) {
        if (!serial_heartbeat_timeout_sent && serial_has_received_frame) {
            serial_heartbeat_timeout_sent = 1;
            serial_heartbeat_restored     = 0;

            frame_parser_reset();
            uart_dma_rx_flush(hUart);

            display_show_failure(REASON_TIMEOUT);
            led_timeout_start();
        }
    }
}

/* -------- 帧解析字节间超时检查（5ms 定时器） -------- */
static void vFrameTimeoutTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    frame_parser_check_timeout(xTaskGetTickCount());
}

/* -------- IWDG 初始化（~2s 溢出，1250 × 64 / 40kHz） -------- */
static void IWDG_Init(void)
{
    RCC_LSICmd(ENABLE);
    while (!RCC_GetFlagStatus(RCC_FLAG_LSIRDY));
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_256);
    IWDG_SetReload(625);   /* 40kHz / 256 * 625 = 4s */
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/* -------- UART RX 任务 --------
   轮询 DMA ringbuffer → 逐字节喂帧解析器 → 完整帧交给 cmd_handler。
   喂狗在此任务的循环尾部执行——此任务卡死=系统真死, 狗应该咬。 */
static void vUartRxTask(void *pvParameters)
{
    (void)pvParameters;

    frame_parser_reset();
    uart_dma_set_rx_task(hUart, xTaskGetCurrentTaskHandle());
    xLastFrameTicks = xTaskGetTickCount();

    for (;;) {
        uint16_t avail;
        avail = uart_dma_rx_available(hUart);

        /* 等待数据，500ms 超时喂狗 */
        if (avail == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        }

        avail = uart_dma_rx_available(hUart);

        /* 逐字节吐出 ringbuffer，喂帧解析器（字节间超时由 5ms 定时器独立处理） */
        while (uart_dma_rx_available(hUart)) {
            uint8_t ch;
            ParsedFrame_t frame;
            uart_dma_recv(hUart, &ch, 1);
            if (frame_parser_feed(ch, xTaskGetTickCount(), &frame)) {
                dispatch_frame(&frame);
            }
        }

        /* 帧解码/校验错误 */
        if (frame_parser_had_error()) {
            /* 仅记录, 无 LED */
        }

        /* ── 喂狗: 此任务循环走完 = 核心路径正常 ── */
        IWDG_ReloadCounter();
    }
}

/* -------- FreeRTOS 钩子 -------- */
void vApplicationIdleHook(void) {}
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)  { (void)file; (void)line; for (;;); }
#endif

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

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) { (void)xTask; (void)pcTaskName; for (;;); }
void vApplicationMallocFailedHook(void)                                    { for (;;); }

/* -------- 入口 -------- */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* 初始化 SPI Flash + 字库 (必须在 LCD 显示汉字之前) */
    led_init();
    norflash_init();
    fonts_init();

    /* 初始化 TFTLCD 屏 (启动画面需要 Flash 字库) */
    display_init();

    /* 打开 DMA-UART */
    hUart = uart_dma_open(UART_PORT1, 115200,
                          uart_tx_rb_buf, sizeof(uart_tx_rb_buf),
                          uart_rx_rb_buf, sizeof(uart_rx_rb_buf));
    configASSERT(hUart);

    /* 启动横幅 */
    uart_send_text_line("");
    uart_send_text_line("========================================");
    uart_send_text_line("  FaceRecognition STM32 v1.0");
    uart_send_text_line("  DMA+RingBuffer UART / FreeRTOS");
    uart_send_text_line("========================================");
    uart_send_text_line("[1/5] GPIO OK");
    uart_send_text_line("[2/5] TFTLCD OK");
    uart_send_text_line("[3/5] UART DMA OK (USART1, 115200)");
    uart_send_text_line("[4/5] FreeRTOS tasks creating...");

    /* RX 任务 */
    xTaskCreate(vUartRxTask, "UartRx", configMINIMAL_STACK_SIZE * 2,
                NULL, 3, &xUartRxTaskHandle);

    /* 1s 心跳检查定时器 */
    xHeartbeatTimer = xTimerCreate("HBCheck", pdMS_TO_TICKS(1000), pdTRUE, NULL, vHeartbeatTimerCallback);
    configASSERT(xHeartbeatTimer);
    xTimerStart(xHeartbeatTimer, 0);

    /* 5ms 帧解析超时定时器 */
    xFrameTimeoutTimer = xTimerCreate("FrameTO", pdMS_TO_TICKS(5), pdTRUE, NULL, vFrameTimeoutTimerCallback);
    configASSERT(xFrameTimeoutTimer);
    xTimerStart(xFrameTimeoutTimer, 0);

    IWDG_Init();

    vTaskStartScheduler();
    for (;;);
}
