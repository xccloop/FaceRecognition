#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Task handles */
static TaskHandle_t xUartRxTaskHandle = NULL;

/* UART receive queue — non-static so ISR in stm32f10x_it.c can access it */
QueueHandle_t xUartRxQueue = NULL;

/* USART2 init: PA2(TX) PA3(RX), 115200-8-N-1, RXNE interrupt */
static void USART2_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    /* PA2 = TX, alternate-function push-pull */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* PA3 = RX, floating input */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = baudrate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART2, &USART_InitStructure);

    /* Enable RXNE interrupt */
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    /* NVIC: preemption priority 13 (numeric, i.e. low urgency) so ISR
       can safely call FreeRTOS API (must be >= configMAX_SYSCALL_INTERRUPT_PRIORITY=11) */
    NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 13;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART2, ENABLE);
}

/* PA0 init: push-pull output for indicator LED */
static void PA0_LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_ResetBits(GPIOA, GPIO_Pin_0);
}

/*
 * UART receive task: parse 8-byte frame from Transmit.txt sender
 *   Byte 0-1: header  0xAA 0x55
 *   Byte 2-5: count   uint32 little-endian
 *   Byte 6-7: checksum (sum of bytes 0..5) uint16 little-endian
 * On valid frame: light PA0, echo frame back
 */
static void vUartRxTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t ch;
    uint8_t buf[8];
    uint8_t idx = 0;

    for (;;)
    {
        if (xQueueReceive(xUartRxQueue, &ch, portMAX_DELAY) == pdPASS)
        {
            /* State machine: sync to header then collect frame */
            switch (idx)
            {
            case 0:
                if (ch == 0xAA) buf[idx++] = ch;
                break;
            case 1:
                if (ch == 0x55) buf[idx++] = ch;
                else { idx = 0; if (ch == 0xAA) buf[idx++] = ch; }
                break;
            case 2: case 3: case 4: case 5:
                buf[idx++] = ch;
                break;
            case 6:
                buf[idx++] = ch;
                break;
            case 7:
                buf[7] = ch;
                idx = 0;

                /* Verify checksum */
                {
                    uint16_t sum = 0;
                    uint8_t i;
                    for (i = 0; i < 6; i++) sum += buf[i];

                    uint16_t rx_checksum = buf[6] | ((uint16_t)buf[7] << 8);

                    if (sum == rx_checksum)
                    {
                        GPIO_SetBits(GPIOA, GPIO_Pin_0); /* Light PA0 */
                    }
                }
                break;
            }
        }
    }
}

/* Assert failed callback (required by FWlib when USE_FULL_ASSERT enabled) */
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    for (;;) {}
}
#endif

/*-----------------------------------------------------------*/
/* FreeRTOS hook functions required by FreeRTOSConfig.h       */
/*-----------------------------------------------------------*/

/* configSUPPORT_STATIC_ALLOCATION == 1 */
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

/* configCHECK_FOR_STACK_OVERFLOW == 2 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    for (;;) {}
}

/* configUSE_MALLOC_FAILED_HOOK == 1 */
void vApplicationMallocFailedHook(void)
{
    for (;;) {}
}

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    BaseType_t xResult;

    /* Init PA0 LED (before UART in case PA clock already enabled there) */
    PA0_LED_Init();

    /* Create UART receive queue (hold up to 64 bytes) */
    xUartRxQueue = xQueueCreate(64, sizeof(uint8_t));
    configASSERT(xUartRxQueue != NULL);

    /* Init USART2 */
    USART2_Init(115200);

    xResult = xTaskCreate(vUartRxTask,
                          "UartRx",
                          configMINIMAL_STACK_SIZE,
                          NULL,
                          3,
                          &xUartRxTaskHandle);
    configASSERT(xResult == pdPASS);

    vTaskStartScheduler();

    for (;;) {}
}
