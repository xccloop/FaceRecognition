#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "uart_dma.h"

/* ================================================================
 *
 *  帧协议使用说明 Frame Protocol Quick Reference
 *
 *  帧格式 Frame Format:
 *  ┌──────┬─────┬─────┬───────┬───────┬──────────────┬─────┬──────┐
 *  │ 0xAA │ CMD │ DIR │ LEN_H │ LEN_L │ DATA (转义)  │ XOR │ 0x55 │
 *  └──────┴─────┴─────┴───────┴───────┴──────────────┴─────┴──────┘
 *   帧头                    大端长度              校验  帧尾
 *
 *  方向 DIR: 0x01 = Pi/PC → STM32,  0x02 = STM32 → Pi/PC
 *
 *  转义 Escape (仅 DATA 段):
 *    0xAA → 0xBB 0x55    0x55 → 0xBB 0xAA    0xBB → 0xBB 0x44
 *
 *  XOR = CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1]
 *  (帧头 0xAA 和帧尾 0x55 不参与 XOR)
 *
 *  ── PC 串口助手测试命令 (HEX 模式发送) ─────────────────────
 *
 *  心跳:
 *    AA 1F 01 00 00 1E 55
 *    预期回复: [OK] HEARTBEAT received + 二进制 ACK 帧
 *
 *  识别成功(有人):
 *    AA 10 01 00 00 11 55
 *    预期回复: [OK] IDENTIFY: face recognized!
 *
 *  未注册人脸:
 *    AA 11 01 00 00 10 55
 *    预期回复: [WARN] UNKNOWN face detected!
 *
 *  无人脸:
 *    AA 12 01 00 00 13 55
 *    预期回复: [INFO] NOFACE - standby
 *
 *  多人脸:
 *    AA 13 01 00 00 12 55
 *    预期回复: [WARN] MULTIFACE: multiple faces detected!
 *
 *  ★ 务必使用 HEX(十六进制)发送模式，不能用 ASCII 文本模式
 *
 * ================================================================ */

/* ── 从 uart_port.c 导入的全局 IDLE 计数器 ── */
extern volatile uint32_t g_idle_count;

/* ================================================================
 * 帧协议常量
 * ================================================================ */
#define FRAME_HEADER        0xAA
#define FRAME_TAIL          0x55
#define ESCAPE_BYTE         0xBB
#define ESCAPE_XOR_A        0x55   /* 0xBB 0x55 → 0xAA */
#define ESCAPE_XOR_B        0xAA   /* 0xBB 0xAA → 0x55 */
#define ESCAPE_XOR_SELF     0x44   /* 0xBB 0x44 → 0xBB */

#define DIR_PI_TO_STM32     0x01
#define DIR_STM32_TO_PI     0x02

#define CMD_IDENTIFY        0x10   /* 识别成功 */
#define CMD_UNKNOWN         0x11   /* 检测到人脸但未识别 */
#define CMD_NOFACE          0x12   /* 无人脸 */
#define CMD_MULTIFACE       0x13   /* 多人脸 */
#define CMD_HEARTBEAT       0x1F   /* 心跳 */
#define CMD_ACK             0x20   /* STM32→Pi: 确认 */

#define MAX_DATA_LEN        64
#define HEARTBEAT_TIMEOUT_MS 15000

/* 帧解析状态 */
typedef enum {
    STATE_SYNC,           /* 搜索帧头 0xAA */
    STATE_CMD,
    STATE_DIR,
    STATE_LEN_H,
    STATE_LEN_L,
    STATE_DATA,           /* 读取 LEN 字节，含转义处理 */
    STATE_XOR,
    STATE_TAIL
} FrameState_t;

/* 解析出的帧结构 */
typedef struct {
    uint8_t  cmd;
    uint8_t  dir;
    uint16_t len;
    uint8_t  data[MAX_DATA_LEN];
} ParsedFrame_t;

/* ================================================================
 * 句柄与全局变量
 * ================================================================ */
static TaskHandle_t  xUartRxTaskHandle  = NULL;
static TimerHandle_t xHeartbeatTimer   = NULL;
static TimerHandle_t xWatchdogTimer    = NULL;

/* DMA-UART 驱动句柄 */
static uart_dma_handle_t *hUart = NULL;

/* DMA ringbuffer 缓冲区（须为 2 的幂，且由调用者静态分配） */
static uint8_t uart_tx_rb_buf[1024];
static uint8_t uart_rx_rb_buf[1024];

/* 心跳时间戳 — 由接收任务更新，定时器检查 */
static TickType_t xLastFrameTicks = 0;

/* 串口诊断输出计数器（只发一次避免刷屏） */
static uint8_t serial_heartbeat_timeout_sent = 0;
static uint8_t serial_heartbeat_restored     = 0;

/* ================================================================
 * GPIO 初始化（LED 保留 HW 初始化，但不用于状态指示）
 * ================================================================ */

static void LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOE, ENABLE);

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    GPIO_SetBits(GPIOB, GPIO_Pin_5);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_Init(GPIOE, &GPIO_InitStructure);
    GPIO_SetBits(GPIOE, GPIO_Pin_5);
}

/* ================================================================
 * 裸文本串口输出（绕过帧协议，给人看的调试信息）
 * ================================================================ */
static void uart_send_text(const char *msg)
{
    uint16_t len = 0;
    while (msg[len]) len++;
    if (len > 0 && hUart)
        uart_dma_send(hUart, (const uint8_t *)msg, len);
}

static void uart_send_text_line(const char *msg)
{
    uart_send_text(msg);
    uart_send_text("\r\n");
}

/* ================================================================
 * 直接寄存器串口调试输出 — 完全绕过 DMA/RingBuffer/FreeRTOS
 * 用于任务存活确认和预调度器诊断
 * ================================================================ */
static void uart_direct_tx(const char *s)
{
    while (*s) {
        while (!(USART1->SR & USART_SR_TXE));
        USART1->DR = *s++;
    }
}

/* ================================================================
 * 帧编码（转义 + 组帧）
 * ================================================================ */

static uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                             const uint8_t *data, uint16_t data_len,
                             uint8_t *out_buf, uint16_t out_capacity)
{
    uint16_t pos = 0;
    uint8_t  xor_val;
    uint16_t i;

    if (out_capacity < (7 + 2 * data_len))
        return 0;

    out_buf[pos++] = FRAME_HEADER;
    out_buf[pos++] = cmd;
    out_buf[pos++] = dir;
    out_buf[pos++] = (uint8_t)((data_len >> 8) & 0xFF);
    out_buf[pos++] = (uint8_t)(data_len & 0xFF);

    for (i = 0; i < data_len; i++)
    {
        uint8_t b = data[i];
        if (b == FRAME_HEADER)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_A; }
        else if (b == FRAME_TAIL)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_B; }
        else if (b == ESCAPE_BYTE)
            { out_buf[pos++] = ESCAPE_BYTE; out_buf[pos++] = ESCAPE_XOR_SELF; }
        else
            out_buf[pos++] = b;
    }

    xor_val = cmd ^ dir
            ^ (uint8_t)((data_len >> 8) & 0xFF)
            ^ (uint8_t)(data_len & 0xFF);
    for (i = 0; i < data_len; i++)
        xor_val ^= data[i];
    out_buf[pos++] = xor_val;
    out_buf[pos++] = FRAME_TAIL;

    return pos;
}

static void uart_send_frame(uint8_t cmd, uint8_t dir,
                            const uint8_t *data, uint16_t data_len)
{
    uint8_t  buf[7 + 2 * MAX_DATA_LEN];
    uint16_t total;
    total = frame_encode(cmd, dir, data, data_len, buf, sizeof(buf));
    if (total > 0 && hUart)
        uart_dma_send(hUart, buf, total);
}

/* ================================================================
 * 帧解码 — 反转义
 * ================================================================ */

static uint8_t unescape_byte(uint8_t b, uint8_t *out, uint8_t *esc_state)
{
    if (*esc_state)
    {
        *esc_state = 0;
        if (b == ESCAPE_XOR_A)         *out = FRAME_HEADER;
        else if (b == ESCAPE_XOR_B)    *out = FRAME_TAIL;
        else if (b == ESCAPE_XOR_SELF) *out = ESCAPE_BYTE;
        else return 0;
        return 1;
    }
    if (b == ESCAPE_BYTE) { *esc_state = 1; return 0; }
    *out = b;
    return 1;
}

/* ================================================================
 * 命令 Handler
 * ================================================================ */

static void handle_identify(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[OK] IDENTIFY: face recognized!");
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, NULL, 0);
}

static void handle_unknown(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[WARN] UNKNOWN face detected!");
}

static void handle_noface(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[INFO] NOFACE - standby");
}

static void handle_multiface(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_text_line("[WARN] MULTIFACE: multiple faces detected!");
}

static void handle_heartbeat(const ParsedFrame_t *f)
{
    (void)f;
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, (const uint8_t *)"HB", 2);
    uart_send_text_line("[OK] HEARTBEAT received");
}

static void dispatch_frame(const ParsedFrame_t *f)
{
    if (f->dir != DIR_PI_TO_STM32)
        return;

    xLastFrameTicks = xTaskGetTickCount();

    if (serial_heartbeat_timeout_sent && !serial_heartbeat_restored)
    {
        uart_send_text_line("[OK] HEARTBEAT RESTORED - Pi back online");
        serial_heartbeat_restored     = 1;
        serial_heartbeat_timeout_sent = 0;
    }

    switch (f->cmd)
    {
    case CMD_IDENTIFY:   handle_identify(f);   break;
    case CMD_UNKNOWN:    handle_unknown(f);    break;
    case CMD_NOFACE:     handle_noface(f);     break;
    case CMD_MULTIFACE:  handle_multiface(f);  break;
    case CMD_HEARTBEAT:  handle_heartbeat(f);  break;
    default: break;
    }
}

/* ================================================================
 * 心跳超时定时器回调
 * ================================================================ */

static void vHeartbeatTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    TickType_t now  = xTaskGetTickCount();
    TickType_t diff = (now - xLastFrameTicks) * portTICK_PERIOD_MS;

    if (diff > HEARTBEAT_TIMEOUT_MS)
    {
        if (!serial_heartbeat_timeout_sent)
        {
            uart_send_text_line("[ERROR] HEARTBEAT TIMEOUT (>15s) - Pi may be offline!");
            serial_heartbeat_timeout_sent = 1;
            serial_heartbeat_restored     = 0;
        }
    }
}

/* ================================================================
 * 看门狗喂狗定时器回调
 * ================================================================ */

static void vWatchdogTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    IWDG_ReloadCounter();
}

/* ================================================================
 * IWDG 独立看门狗初始化
 * ================================================================ */

static void IWDG_Init(void)
{
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(1250);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/* ================================================================
 * UART 接收任务（DMA + RingBuffer 驱动）
 * ================================================================ */

static void vUartRxTask(void *pvParameters)
{
    /* T0: 函数第一条语句 — 直接寄存器输出，确认任务入口到达 */
    uart_direct_tx("[RX] START\r\n");

    (void)pvParameters;
    FrameState_t  state      = STATE_SYNC;
    ParsedFrame_t frame;
    uint16_t      data_idx   = 0;
    uint8_t       xor_calc   = 0;
    uint8_t       esc_state  = 0;

    uart_dma_set_rx_task(hUart, xTaskGetCurrentTaskHandle());
    xLastFrameTicks = xTaskGetTickCount();

    for (;;)
    {
        uint16_t avail;

        /* 5 秒超时轮询替代永久阻塞 — 即使无数据也能打印存活信息 */
        avail = uart_dma_rx_available(hUart);
        if (avail == 0) {
            uint32_t notify_val = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            if (notify_val == 0) {
                /* 5 秒内无通知 — 超时：打印 RX 任务存活 + idle 计数器 + CNDTR */
                {
                    uint32_t idle = g_idle_count;
                    uint32_t cndtr = uart_port_rx_dma_remaining(UART_PORT1);
                    char diag[48];
                    int di = 0;
                    char *p = "[DIAG] alive idle=";
                    while (*p) diag[di++] = *p++;
                    if (idle >= 100)      diag[di++] = '0' + (idle/100);
                    if (idle >= 10)       diag[di++] = '0' + ((idle/10)%10);
                    diag[di++] = '0' + (idle%10);
                    p = " cndtr=";
                    while (*p) diag[di++] = *p++;
                    if (cndtr >= 1000)    diag[di++] = '0' + (cndtr/1000);
                    if (cndtr >= 100)     diag[di++] = '0' + ((cndtr/100)%10);
                    if (cndtr >= 10)      diag[di++] = '0' + ((cndtr/10)%10);
                    diag[di++] = '0' + (cndtr%10);
                    p = "\r\n";
                    while (*p) diag[di++] = *p++;
                    uart_dma_send(hUart, (uint8_t*)diag, di);
                }
            }
        }

        avail = uart_dma_rx_available(hUart);
        if (avail > 0)
        {
            static TickType_t xLastRxDiag = 0;
            TickType_t now = xTaskGetTickCount();
            if ((now - xLastRxDiag) >= pdMS_TO_TICKS(200))
            {
                xLastRxDiag = now;
                /* 构建 "[RX: XX bytes]" */
                uint8_t diag[18];
                uint8_t d = 0;
                diag[d++] = '\r'; diag[d++] = '\n';
                diag[d++] = '[';  diag[d++] = 'R'; diag[d++] = 'X';
                diag[d++] = ':';  diag[d++] = ' ';
                if (avail >= 10) diag[d++] = '0' + (avail / 10);
                diag[d++] = '0' + (avail % 10);
                diag[d++] = ' ';  diag[d++] = 'b'; diag[d++] = 'y';
                diag[d++] = 't';  diag[d++] = 'e'; diag[d++] = 's';
                diag[d++] = ']';
                diag[d++] = '\r'; diag[d++] = '\n';
                uart_dma_send(hUart, diag, d);
            }
        }

        while (uart_dma_rx_available(hUart))
        {
            uint8_t ch;
            uart_dma_recv(hUart, &ch, 1);

            switch (state)
            {
            case STATE_SYNC:
                if (ch == FRAME_HEADER)
                    state = STATE_CMD;
                break;

            case STATE_CMD:
                frame.cmd  = ch;
                xor_calc   = ch;
                state      = STATE_DIR;
                break;

            case STATE_DIR:
                frame.dir  = ch;
                xor_calc  ^= ch;
                state      = STATE_LEN_H;
                break;

            case STATE_LEN_H:
                frame.len  = ((uint16_t)ch) << 8;
                xor_calc  ^= ch;
                state      = STATE_LEN_L;
                break;

            case STATE_LEN_L:
                frame.len |= ch;
                xor_calc  ^= ch;
                if (frame.len > MAX_DATA_LEN)
                {
                    uart_send_text_line("[ERR] Frame parse fail: LEN exceeds MAX_DATA_LEN");
                    state    = STATE_SYNC;
                    esc_state = 0;
                    break;
                }
                data_idx   = 0;
                esc_state  = 0;
                state      = (frame.len > 0) ? STATE_DATA : STATE_XOR;
                break;

            case STATE_DATA:
            {
                uint8_t raw_byte;
                if (unescape_byte(ch, &raw_byte, &esc_state))
                {
                    frame.data[data_idx++] = raw_byte;
                    xor_calc              ^= raw_byte;
                    if (data_idx >= frame.len)
                        state = STATE_XOR;
                }
                break;
            }

            case STATE_XOR:
                if (ch != xor_calc)
                {
                    uart_send_text_line("[ERR] Frame parse fail: XOR mismatch");
                    state    = STATE_SYNC;
                    esc_state = 0;
                    break;
                }
                state = STATE_TAIL;
                break;

            case STATE_TAIL:
                if (ch == FRAME_TAIL)
                {
                    dispatch_frame(&frame);
                }
                else
                {
                    uart_send_text_line("[ERR] Frame parse fail: missing EOF (0x55)");
                }
                state    = STATE_SYNC;
                esc_state = 0;
                break;

            } /* switch(state) */
        } /* while available */
    }
}

/* ================================================================
 * 空闲钩子
 * ================================================================ */

void vApplicationIdleHook(void)
{
}

/* ================================================================
 * Assert 失败回调
 * ================================================================ */
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file; (void)line;
    for (;;) {}
}
#endif

/* ================================================================
 * FreeRTOS 静态内存分配钩子
 * ================================================================ */

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

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    for (;;) {}
}

/* ================================================================
 * main()
 * ================================================================ */

int main(void)
{
    BaseType_t task_ok;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    LED_Init();

    hUart = uart_dma_open(UART_PORT1, 115200,
                          uart_tx_rb_buf, sizeof(uart_tx_rb_buf),
                          uart_rx_rb_buf, sizeof(uart_rx_rb_buf));
    configASSERT(hUart != NULL);

    /* 启动横幅 */
    uart_send_text_line("");
    uart_send_text_line("========================================");
    uart_send_text_line("  FaceRecognition STM32 v1.0");
    uart_send_text_line("  DMA+RingBuffer UART / FreeRTOS");
    uart_send_text_line("========================================");
    uart_send_text_line("[1/4] GPIO OK");
    uart_send_text_line("[2/4] UART DMA OK (USART1, 115200)");
    uart_send_text_line("[3/4] FreeRTOS tasks creating...");

    /* ── 创建 RX 任务，显式检查返回值 ── */
    task_ok = xTaskCreate(vUartRxTask, "UartRx",
                          configMINIMAL_STACK_SIZE * 2, NULL,
                          3, &xUartRxTaskHandle);

    /* T1: 直接寄存器输出 — 确认 xTaskCreate 结果 + TX 硬件在 banner 后仍可用 */
    if (task_ok == pdPASS) {
        uart_direct_tx("[T1] xTaskCreate OK (handle=");
        if (xUartRxTaskHandle != NULL) {
            uart_direct_tx("valid)\r\n");
        } else {
            uart_direct_tx("NULL!)\r\n");
        }
    } else {
        uart_direct_tx("[FATAL] xTaskCreate FAILED!\r\n");
    }

    xHeartbeatTimer = xTimerCreate("HBCheck",
                                   pdMS_TO_TICKS(1000),
                                   pdTRUE, NULL,
                                   vHeartbeatTimerCallback);
    configASSERT(xHeartbeatTimer != NULL);
    xTimerStart(xHeartbeatTimer, 0);

    xWatchdogTimer = xTimerCreate("WdgFeed",
                                  pdMS_TO_TICKS(500),
                                  pdTRUE, NULL,
                                  vWatchdogTimerCallback);
    configASSERT(xWatchdogTimer != NULL);
    xTimerStart(xWatchdogTimer, 0);

    IWDG_Init();

    uart_send_text_line("[4/4] Starting scheduler...");
    uart_send_text_line("========================================");
    uart_direct_tx("[MAIN] vTaskStartScheduler() now\r\n");

    vTaskStartScheduler();

    for (;;) {}
}
