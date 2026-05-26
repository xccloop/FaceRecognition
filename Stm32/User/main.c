#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "uart_dma.h"
#include "frame_protocol.h"
#include "cmd_handler.h"

extern volatile uint32_t g_idle_count;

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

/* -------- LED -------- */
#define LED_ERR_PORT  GPIOB
#define LED_ERR_PIN   GPIO_Pin_5
#define LED_OK_PORT   GPIOE
#define LED_OK_PIN    GPIO_Pin_5

static void LED_Init(void)
{
    GPIO_InitTypeDef g;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOE, ENABLE);
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Pin   = GPIO_Pin_5;
    GPIO_Init(GPIOB, &g);  GPIO_SetBits(GPIOB, GPIO_Pin_5);  /* 灭 (高电平) */
    GPIO_Init(GPIOE, &g);  GPIO_SetBits(GPIOE, GPIO_Pin_5);
}

/* 启动闪烁: 两灯同时亮灭 N 次，每次 ~150ms */
static void LED_StartupFlash(int times)
{
    int i;
    for (i = 0; i < times; i++) {
        GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);
        GPIO_ResetBits(LED_OK_PORT,  LED_OK_PIN);
        vTaskDelay(pdMS_TO_TICKS(150));
        GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
        GPIO_SetBits(LED_OK_PORT,  LED_OK_PIN);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* 帧错误快闪: PB5 短促闪烁 N 次 */
static void LED_ErrorFlash(int times)
{
    int i;
    for (i = 0; i < times; i++) {
        GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);
        vTaskDelay(pdMS_TO_TICKS(80));
        GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

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

/* 直接写 USART 寄存器，完全绕过 DMA/RTOS。
   用于确认任务入口和启动阶段的硬件状态。 */
static void uart_direct_tx(const char *s)
{
    while (*s) {
        while (!(USART1->SR & USART_SR_TXE));
        USART1->DR = *s++;
    }
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
    if ((xTaskGetTickCount() - xLastFrameTicks) * portTICK_PERIOD_MS > 15000) {
        if (!serial_heartbeat_timeout_sent) {
            uart_send_text_line("[ERROR] HEARTBEAT TIMEOUT (>15s) - Pi may be offline!");
            serial_heartbeat_timeout_sent = 1;
            serial_heartbeat_restored     = 0;

            /* 三重恢复: 复位解析器 + 清空缓冲 + 亮错误灯 */
            frame_parser_reset();
            uart_dma_rx_flush(hUart);
            GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);  /* PB5 常亮 */
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
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(1250);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/* -------- UART RX 任务 --------
   轮询 DMA ringbuffer → 逐字节喂帧解析器 → 完整帧交给 cmd_handler。
   喂狗在此任务的循环尾部执行——此任务卡死=系统真死, 狗应该咬。 */
static void vUartRxTask(void *pvParameters)
{
    uart_direct_tx("[RX] START\r\n");
    (void)pvParameters;

    frame_parser_reset();
    uart_dma_set_rx_task(hUart, xTaskGetCurrentTaskHandle());
    xLastFrameTicks = xTaskGetTickCount();

    for (;;) {
        uint16_t avail;
        avail = uart_dma_rx_available(hUart);

        /* 等待数据，5 秒超时打印存活诊断 */
        if (avail == 0) {
            uint32_t notify = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            if (notify == 0) {
                uint32_t idle  = g_idle_count;
                uint32_t cndtr = uart_port_rx_dma_remaining(UART_PORT1);
                char buf[48];
                int i = 0;
                char *p = "[DIAG] alive idle=";
                while (*p) buf[i++] = *p++;
                if (idle >= 100)  buf[i++] = '0' + (idle / 100);
                if (idle >= 10)   buf[i++] = '0' + ((idle / 10) % 10);
                buf[i++] = '0' + (idle % 10);
                p = " cndtr=";
                while (*p) buf[i++] = *p++;
                if (cndtr >= 1000) buf[i++] = '0' + (cndtr / 1000);
                if (cndtr >= 100)  buf[i++] = '0' + ((cndtr / 100) % 10);
                if (cndtr >= 10)   buf[i++] = '0' + ((cndtr / 10) % 10);
                buf[i++] = '0' + (cndtr % 10);
                p = "\r\n";
                while (*p) buf[i++] = *p++;
                uart_dma_send(hUart, (uint8_t *)buf, i);
            }
        }

        avail = uart_dma_rx_available(hUart);
        /* 收到数据时打印字节数（每 200ms 最多一次，避免刷屏） */
        if (avail > 0) {
            static TickType_t xLastRxDiag = 0;
            TickType_t now = xTaskGetTickCount();
            if ((now - xLastRxDiag) >= pdMS_TO_TICKS(200)) {
                xLastRxDiag = now;
                uint8_t d[18]; uint8_t di = 0;
                d[di++] = '\r'; d[di++] = '\n';
                d[di++] = '[';  d[di++] = 'R'; d[di++] = 'X';
                d[di++] = ':';  d[di++] = ' ';
                if (avail >= 10) d[di++] = '0' + (avail / 10);
                d[di++] = '0' + (avail % 10);
                d[di++] = ' ';  d[di++] = 'b'; d[di++] = 'y';
                d[di++] = 't';  d[di++] = 'e'; d[di++] = 's';
                d[di++] = ']';
                d[di++] = '\r'; d[di++] = '\n';
                uart_dma_send(hUart, d, di);
            }
        }

        /* 逐字节吐出 ringbuffer，喂帧解析器（字节间超时由 5ms 定时器独立处理） */
        while (uart_dma_rx_available(hUart)) {
            uint8_t ch;
            ParsedFrame_t frame;
            uart_dma_recv(hUart, &ch, 1);
            if (frame_parser_feed(ch, xTaskGetTickCount(), &frame)) {
                dispatch_frame(&frame);
            }
        }

        /* 帧解码/校验错误 → PB5 快闪 3 下 */
        if (frame_parser_had_error()) {
            LED_ErrorFlash(3);
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
    LED_Init();

    /* 启动闪烁: 两灯同时亮灭 2 次，表示上电初始化 */
    {
        int i;
        for (i = 0; i < 2; i++) {
            GPIO_ResetBits(LED_ERR_PORT, LED_ERR_PIN);
            GPIO_ResetBits(LED_OK_PORT,  LED_OK_PIN);
            {
                volatile uint32_t d = 600000; while (d--) __NOP();
            }
            GPIO_SetBits(LED_ERR_PORT, LED_ERR_PIN);
            GPIO_SetBits(LED_OK_PORT,  LED_OK_PIN);
            {
                volatile uint32_t d = 600000; while (d--) __NOP();
            }
        }
    }

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
    uart_send_text_line("[1/4] GPIO OK");
    uart_send_text_line("[2/4] UART DMA OK (USART1, 115200)");
    uart_send_text_line("[3/4] FreeRTOS tasks creating...");

    /* RX 任务 */
    if (xTaskCreate(vUartRxTask, "UartRx", configMINIMAL_STACK_SIZE * 2,
                    NULL, 3, &xUartRxTaskHandle) != pdPASS)
        uart_direct_tx("[FATAL] xTaskCreate FAILED!\r\n");
    else
        uart_direct_tx("[T1] xTaskCreate OK\r\n");

    /* 1s 心跳检查定时器 */
    xHeartbeatTimer = xTimerCreate("HBCheck", pdMS_TO_TICKS(1000), pdTRUE, NULL, vHeartbeatTimerCallback);
    configASSERT(xHeartbeatTimer);
    xTimerStart(xHeartbeatTimer, 0);

    /* 5ms 帧解析超时定时器 */
    xFrameTimeoutTimer = xTimerCreate("FrameTO", pdMS_TO_TICKS(5), pdTRUE, NULL, vFrameTimeoutTimerCallback);
    configASSERT(xFrameTimeoutTimer);
    xTimerStart(xFrameTimeoutTimer, 0);

    IWDG_Init();

    uart_send_text_line("[4/4] Starting scheduler...");
    uart_send_text_line("========================================");
    uart_direct_tx("[MAIN] vTaskStartScheduler()\r\n");

    vTaskStartScheduler();
    for (;;);
}
