#include "stm32f10x_it.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Extern queue handle — created in main.c, consumed here (ISR) */
extern QueueHandle_t xUartRxQueue;

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

void DebugMon_Handler(void)
{
}

void USART2_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* RX byte received */
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(USART2);
        xQueueSendFromISR(xUartRxQueue, &ch, &xHigherPriorityTaskWoken);
    }

    /* Clear overrun error if present — must read SR then DR to unlock RX */
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        (void)USART_ReceiveData(USART2);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
