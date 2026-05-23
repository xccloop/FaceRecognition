#include "stm32f10x_it.h"
#include "FreeRTOS.h"
#include "task.h"
#include "uart_port.h"

void NMI_Handler(void) {}
void DebugMon_Handler(void) {}

void HardFault_Handler(void)
{
    while (1) {}
}

void MemManage_Handler(void)
{
    while (1) {}
}

void BusFault_Handler(void)
{
    while (1) {}
}

void UsageFault_Handler(void)
{
    while (1) {}
}

/* ── USART1 interrupt (IDLE / error) ── */
void USART1_IRQHandler(void)
{
    /* 状态描述已全部通过串口文本实现，ISR 中不再操作 LED */
    uart_port_irq_handler(UART_PORT1);
}

/* ── DMA1 Channel 5 (USART1 RX: half-transfer + transfer-complete) ── */
void DMA1_Channel5_IRQHandler(void)
{
    uart_port_dma_rx_irq_handler(UART_PORT1);
}

/* ── DMA1 Channel 4 (USART1 TX: transfer-complete) ── */
void DMA1_Channel4_IRQHandler(void)
{
    uart_port_dma_tx_irq_handler(UART_PORT1);
}
