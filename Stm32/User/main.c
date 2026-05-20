#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

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

QueueHandle_t xUartRxQueue = NULL; /* ISR 写入，任务读取 */

/* 心跳时间戳 — 由接收任务更新，定时器检查 */
static TickType_t xLastFrameTicks = 0;

/* LED 闪烁上下文 */
static uint8_t  led_pattern_active = 0;  /* 0=命令控制, 1=心跳超时慢闪 */
static uint8_t  led_blink_phase   = 0;
static TickType_t xLastLedToggle  = 0;

/* ================================================================
 * GPIO 初始化
 * ================================================================ */

/* PA0: 状态指示 LED（推挽输出） */
static void PA0_LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOA, GPIO_Pin_0);
}

/* LED 控制便捷宏 */
#define LED_ON()   GPIO_SetBits(GPIOA, GPIO_Pin_0)
#define LED_OFF()  GPIO_ResetBits(GPIOA, GPIO_Pin_0)
#define LED_TOGGLE() do { \
    if (GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_0)) \
        GPIO_ResetBits(GPIOA, GPIO_Pin_0); \
    else \
        GPIO_SetBits(GPIOA, GPIO_Pin_0); \
} while (0)

/* ================================================================
 * UART 初始化
 * ================================================================ */

static void USART2_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    /* PA2 = TX, alternate-function push-pull */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* PA3 = RX, floating input */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate            = baudrate;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART2, &USART_InitStructure);

    /* Enable RXNE interrupt only — TX 用轮询发送 */
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    /* NVIC: 优先级 13（>= configMAX_SYSCALL_INTERRUPT_PRIORITY=11，
       允许在 ISR 中安全调用 FreeRTOS API） */
    NVIC_InitStructure.NVIC_IRQChannel                   = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 13;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART2, ENABLE);
}

/* ================================================================
 * UART 发送（轮询，阻塞）
 * ================================================================ */

static void uart_send_byte(uint8_t b)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
    USART_SendData(USART2, b);
}

/* 发送原始字节数组 */
static void uart_send(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++)
        uart_send_byte(data[i]);
}

/* ================================================================
 * 帧编码（转义 + 组帧）
 * 返回实际发送的字节数（不含帧头帧尾，含转义展开）
 * ================================================================ */

static uint16_t frame_encode(uint8_t cmd, uint8_t dir,
                             const uint8_t *data, uint16_t data_len,
                             uint8_t *out_buf, uint16_t out_capacity)
{
    uint16_t pos = 0;
    uint8_t  xor_val;
    uint16_t i;

    /* out_capacity 最小需求：帧头(1) + CMD + DIR + LEN_H + LEN_L
       + DATA(最坏 2×data_len) + XOR + 帧尾(1) */
    if (out_capacity < (7 + 2 * data_len))
        return 0;

    /* 帧头 */
    out_buf[pos++] = FRAME_HEADER;   /* 0xAA */

    /* CMD */
    out_buf[pos++] = cmd;

    /* DIR */
    out_buf[pos++] = dir;

    /* LEN 大端序 (H-L) */
    out_buf[pos++] = (uint8_t)((data_len >> 8) & 0xFF);
    out_buf[pos++] = (uint8_t)(data_len & 0xFF);

    /* DATA 区（转义编码） */
    for (i = 0; i < data_len; i++)
    {
        uint8_t b = data[i];
        if (b == FRAME_HEADER)        /* 0xAA → 0xBB 0x55 */
        {
            out_buf[pos++] = ESCAPE_BYTE;
            out_buf[pos++] = ESCAPE_XOR_A;
        }
        else if (b == FRAME_TAIL)     /* 0x55 → 0xBB 0xAA */
        {
            out_buf[pos++] = ESCAPE_BYTE;
            out_buf[pos++] = ESCAPE_XOR_B;
        }
        else if (b == ESCAPE_BYTE)    /* 0xBB → 0xBB 0x44 */
        {
            out_buf[pos++] = ESCAPE_BYTE;
            out_buf[pos++] = ESCAPE_XOR_SELF;
        }
        else
        {
            out_buf[pos++] = b;
        }
    }

    /* XOR 校验: CMD ^ DIR ^ LEN_H ^ LEN_L ^ DATA[0] ^ ... ^ DATA[N-1] */
    xor_val = cmd ^ dir
            ^ (uint8_t)((data_len >> 8) & 0xFF)
            ^ (uint8_t)(data_len & 0xFF);
    for (i = 0; i < data_len; i++)
        xor_val ^= data[i];
    out_buf[pos++] = xor_val;

    /* 帧尾 */
    out_buf[pos++] = FRAME_TAIL;

    return pos; /* 实际帧长度 */
}

/* 便捷封装：编码并直接发送 */
static void uart_send_frame(uint8_t cmd, uint8_t dir,
                            const uint8_t *data, uint16_t data_len)
{
    uint8_t  buf[7 + 2 * MAX_DATA_LEN];
    uint16_t total;

    total = frame_encode(cmd, dir, data, data_len, buf, sizeof(buf));
    if (total > 0)
        uart_send(buf, total);
}

/* ================================================================
 * 帧解码 — 反转义
 * 返回 0 表示未完成转义序列，1 表示输出一个原始字节
 * ================================================================ */

static uint8_t unescape_byte(uint8_t b, uint8_t *out, uint8_t *esc_state)
{
    if (*esc_state)
    {
        /* 前一个字节是 0xBB，当前是转义的第二字节 */
        *esc_state = 0;
        if (b == ESCAPE_XOR_A)         /* 0xBB 0x55 → 0xAA */
            *out = FRAME_HEADER;
        else if (b == ESCAPE_XOR_B)    /* 0xBB 0xAA → 0x55 */
            *out = FRAME_TAIL;
        else if (b == ESCAPE_XOR_SELF) /* 0xBB 0x44 → 0xBB */
            *out = ESCAPE_BYTE;
        else
            return 0; /* 非法转义序列，丢弃 */
        return 1;
    }

    if (b == ESCAPE_BYTE)
    {
        *esc_state = 1;
        return 0; /* 等待下一个字节 */
    }

    *out = b;
    return 1;
}

/* ================================================================
 * 命令 Handler
 * ================================================================ */

static void handle_identify(const ParsedFrame_t *f)
{
    /* 数据格式: "001,张三,0.92" */
    led_pattern_active = 0;
    LED_ON();   /* 绿灯常亮（识别成功） */

    /* 回复 ACK 给 Pi */
    uart_send_frame(CMD_ACK, DIR_STM32_TO_PI, NULL, 0);
}

static void handle_unknown(const ParsedFrame_t *f)
{
    /* 陌生人告警：LED 快闪 3 次 */
    uint8_t i;
    led_pattern_active = 0;
    for (i = 0; i < 3; i++)
    {
        LED_ON();
        vTaskDelay(pdMS_TO_TICKS(150));
        LED_OFF();
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void handle_noface(const ParsedFrame_t *f)
{
    /* 待机：LED 熄灭 */
    led_pattern_active = 0;
    LED_OFF();
}

static void handle_multiface(const ParsedFrame_t *f)
{
    /* 多人进入告警：LED 快速闪烁 */
    uint8_t i;
    led_pattern_active = 0;
    for (i = 0; i < 6; i++)
    {
        LED_ON();
        vTaskDelay(pdMS_TO_TICKS(80));
        LED_OFF();
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void handle_heartbeat(const ParsedFrame_t *f)
{
    /* 心跳：短闪一次，更新时间戳 */
    LED_ON();
    vTaskDelay(pdMS_TO_TICKS(30));
    LED_OFF();

    led_pattern_active = 0;
}

/* 命令分发 */
static void dispatch_frame(const ParsedFrame_t *f)
{
    if (f->dir != DIR_PI_TO_STM32)
        return; /* 忽略非 Pi 发来的帧（或自己的回声） */

    /* 更新心跳时间戳（任何有效帧都算活动） */
    xLastFrameTicks = xTaskGetTickCount();

    /* 从心跳超时慢闪恢复 */
    if (led_pattern_active)
        led_pattern_active = 0;

    switch (f->cmd)
    {
    case CMD_IDENTIFY:   handle_identify(f);   break;
    case CMD_UNKNOWN:    handle_unknown(f);    break;
    case CMD_NOFACE:     handle_noface(f);     break;
    case CMD_MULTIFACE:  handle_multiface(f);  break;
    case CMD_HEARTBEAT:  handle_heartbeat(f);  break;
    default:
        break; /* 未知命令，静默忽略 */
    }
}

/* ================================================================
 * 心跳超时定时器回调
 * 每 1 秒触发，检查最后一次收帧是否超时
 * ================================================================ */

static void vHeartbeatTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;

    TickType_t now  = xTaskGetTickCount();
    TickType_t diff = (now - xLastFrameTicks) * portTICK_PERIOD_MS;

    if (diff > HEARTBEAT_TIMEOUT_MS)
    {
        /* Pi 离线：进入慢闪告警（1Hz） */
        led_pattern_active = 1;
    }
}

/* ================================================================
 * 看门狗喂狗定时器回调
 * 每 500ms 触发
 * ================================================================ */

static void vWatchdogTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    IWDG_ReloadCounter();
}

/* ================================================================
 * IWDG 独立看门狗初始化
 * LSI 40kHz, 预分频 64 → 625Hz, 重载值 1250 → 2 秒超时
 * ================================================================ */

static void IWDG_Init(void)
{
    /* 使能 LSI（如果尚未使能） */
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);

    /* 使能写访问 */
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);

    /* 预分频 64: 40kHz / 64 = 625Hz */
    IWDG_SetPrescaler(IWDG_Prescaler_64);

    /* 重载值 1250 → 1250 / 625 = 2 秒超时 */
    IWDG_SetReload(1250);

    /* 重载计数器（初始值载入） */
    IWDG_ReloadCounter();

    /* 使能 IWDG */
    IWDG_Enable();
}

/* ================================================================
 * UART 接收任务
 * 
 * 两阶段状态机：
 *   阶段 1 (STATE_SYNC) — 循环读字节直到遇到 0xAA
 *   阶段 2 (STATE_CMD → STATE_TAIL) — 连续解析帧字段
 *   任一状态出错 → 回到 STATE_SYNC
 * ================================================================ */

static void vUartRxTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t       ch;
    FrameState_t  state      = STATE_SYNC;
    ParsedFrame_t frame;
    uint16_t      data_idx   = 0;
    uint8_t       xor_calc   = 0;
    uint8_t       esc_state  = 0;

    /* 初始化心跳时间戳 */
    xLastFrameTicks = xTaskGetTickCount();

    for (;;)
    {
        if (xQueueReceive(xUartRxQueue, &ch, portMAX_DELAY) != pdPASS)
            continue;

        switch (state)
        {

        /* ---- 阶段 1: 搜帧头 ---- */
        case STATE_SYNC:
            if (ch == FRAME_HEADER)
                state = STATE_CMD;
            break;

        /* ---- 阶段 2: 读 CMD ---- */
        case STATE_CMD:
            frame.cmd  = ch;
            xor_calc   = ch;
            state      = STATE_DIR;
            break;

        /* ---- 读 DIR ---- */
        case STATE_DIR:
            frame.dir  = ch;
            xor_calc  ^= ch;
            state      = STATE_LEN_H;
            break;

        /* ---- 读 LEN_H ---- */
        case STATE_LEN_H:
            frame.len  = ((uint16_t)ch) << 8;
            xor_calc  ^= ch;
            state      = STATE_LEN_L;
            break;

        /* ---- 读 LEN_L ---- */
        case STATE_LEN_L:
            frame.len |= ch;
            xor_calc  ^= ch;
            if (frame.len > MAX_DATA_LEN)
            {
                /* 数据长度超限 → 丢弃，重搜帧头 */
                state    = STATE_SYNC;
                esc_state = 0;
                break;
            }
            data_idx   = 0;
            esc_state  = 0;
            state      = (frame.len > 0) ? STATE_DATA : STATE_XOR;
            break;

        /* ---- 读 DATA（含转义） ---- */
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
            /* 转义未完成时留在 STATE_DATA，等下一字节 */
            break;
        }

        /* ---- 读 XOR 校验 ---- */
        case STATE_XOR:
            if (ch != xor_calc)
            {
                /* 校验失败 → 丢弃整帧 */
                state    = STATE_SYNC;
                esc_state = 0;
                break;
            }
            state = STATE_TAIL;
            break;

        /* ---- 读帧尾 0x55 ---- */
        case STATE_TAIL:
            if (ch == FRAME_TAIL)
            {
                /* 完整帧接收成功，分发处理 */
                dispatch_frame(&frame);
            }
            /* 无论帧尾是否匹配，都回到搜帧头 */
            state    = STATE_SYNC;
            esc_state = 0;
            break;

        } /* switch(state) */
    }
}

/* ================================================================
 * 空闲钩子 — LED 慢闪（心跳超时时）
 * ================================================================ */

void vApplicationIdleHook(void)
{
    if (led_pattern_active)
    {
        TickType_t now = xTaskGetTickCount();
        /* 1Hz 闪烁: 500ms 亮, 500ms 灭 */
        if ((now - xLastLedToggle) >= pdMS_TO_TICKS(500))
        {
            xLastLedToggle = now;
            LED_TOGGLE();
        }
    }
}

/* ================================================================
 * Assert 失败回调（USE_FULL_ASSERT 时必需）
 * ================================================================ */
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
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

/* Stack overflow 钩子 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    for (;;) {}
}

/* malloc 失败钩子 */
void vApplicationMallocFailedHook(void)
{
    for (;;) {}
}

/* ================================================================
 * main()
 * ================================================================ */

int main(void)
{
    BaseType_t xResult;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* ---- 硬件初始化 ---- */
    PA0_LED_Init();

    /* 创建 UART 接收队列（64 字节深度） */
    xUartRxQueue = xQueueCreate(64, sizeof(uint8_t));
    configASSERT(xUartRxQueue != NULL);

    USART2_Init(115200);

    /* IWDG 看门狗：2 秒超时 */
    IWDG_Init();

    /* ---- 创建 FreeRTOS 对象 ---- */

    /* UART 接收任务 */
    xResult = xTaskCreate(vUartRxTask, "UartRx",
                          configMINIMAL_STACK_SIZE, NULL,
                          3, &xUartRxTaskHandle);
    configASSERT(xResult == pdPASS);

    /* 心跳超时检测定时器：每 1 秒检查一次 */
    xHeartbeatTimer = xTimerCreate("HBCheck",
                                   pdMS_TO_TICKS(1000),
                                   pdTRUE,   /* 自动重载 */
                                   NULL,
                                   vHeartbeatTimerCallback);
    configASSERT(xHeartbeatTimer != NULL);
    xTimerStart(xHeartbeatTimer, 0);

    /* 看门狗喂狗定时器：每 500ms */
    xWatchdogTimer = xTimerCreate("WdgFeed",
                                  pdMS_TO_TICKS(500),
                                  pdTRUE,   /* 自动重载 */
                                  NULL,
                                  vWatchdogTimerCallback);
    configASSERT(xWatchdogTimer != NULL);
    xTimerStart(xWatchdogTimer, 0);

    /* ---- 启动调度器 ---- */
    vTaskStartScheduler();

    /* 永远不应到达 */
    for (;;) {}
}
