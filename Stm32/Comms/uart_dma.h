/**
 * uart_dma.h — DMA-UART driver (TX chained DMA + RX circular IDLE)
 * ARMCC v5 兼容：includes uart_port.h 提供 stm32f10x.h + bool
 */
#ifndef UART_DMA_H
#define UART_DMA_H

#include "uart_port.h"

typedef struct uart_dma_handle uart_dma_handle_t;

uart_dma_handle_t *uart_dma_open(uint8_t port, uint32_t baud,
    uint8_t *tx_buf, uint32_t tx_size,
    uint8_t *rx_buf, uint32_t rx_size);
void      uart_dma_close(uart_dma_handle_t *h);
uint32_t  uart_dma_send(uart_dma_handle_t *h, const uint8_t *data, uint32_t len);
uint32_t  uart_dma_recv(uart_dma_handle_t *h, uint8_t *buf, uint32_t len);
uint32_t  uart_dma_rx_available(uart_dma_handle_t *h);
uint32_t  uart_dma_tx_free(uart_dma_handle_t *h);
bool      uart_dma_tx_flush(uart_dma_handle_t *h, uint32_t timeout);
void      uart_dma_rx_flush(uart_dma_handle_t *h);
bool      uart_dma_tx_busy(uart_dma_handle_t *h);
void      uart_dma_set_rx_task(uart_dma_handle_t *h, void *task);

#endif
