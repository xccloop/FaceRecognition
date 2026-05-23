/**
 * uart_port.h — Port layer interface for STM32 SPL / CMSIS
 *
 * Adapted from DMA-RingBuffer UART Framework
 * ARMCC v5 兼容：不依赖 <stdbool.h>，C99 bool 手工 typedef
 */
#ifndef UART_PORT_H
#define UART_PORT_H

#include "stm32f10x.h"

#ifndef _STDBOOL_COMPAT_
#define _STDBOOL_COMPAT_
typedef unsigned char bool;
#define false 0
#define true  1
#endif

#define UART_PORT_MAX  3
#define UART_PORT1     0
#define UART_PORT2     1
#define UART_PORT3     2

typedef void (*port_tx_tc_cb_t)(uint8_t port);
typedef void (*port_tx_ht_cb_t)(uint8_t port);
typedef void (*port_rx_tc_cb_t)(uint8_t port);
typedef void (*port_rx_ht_cb_t)(uint8_t port);
typedef void (*port_idle_cb_t)(uint8_t port);
typedef void (*port_error_cb_t)(uint8_t port, uint32_t error);

void uart_port_init(uint8_t port, uint32_t baudrate);
void uart_port_deinit(uint8_t port);

void uart_port_dma_tx_start(uint8_t port, const uint8_t *data, uint32_t len);
void uart_port_dma_rx_start(uint8_t port, uint8_t *buf, uint32_t len);
void uart_port_dma_tx_stop(uint8_t port);
void uart_port_dma_rx_stop(uint8_t port);

uint32_t uart_port_tx_dma_remaining(uint8_t port);
uint32_t uart_port_rx_dma_remaining(uint8_t port);

void uart_port_set_tx_tc_cb(uint8_t port, port_tx_tc_cb_t cb);
void uart_port_set_tx_ht_cb(uint8_t port, port_tx_ht_cb_t cb);
void uart_port_set_rx_tc_cb(uint8_t port, port_rx_tc_cb_t cb);
void uart_port_set_rx_ht_cb(uint8_t port, port_rx_ht_cb_t cb);
void uart_port_set_idle_cb(uint8_t port, port_idle_cb_t cb);
void uart_port_set_error_cb(uint8_t port, port_error_cb_t cb);

void uart_port_irq_handler(uint8_t port);
void uart_port_dma_tx_irq_handler(uint8_t port);
void uart_port_dma_rx_irq_handler(uint8_t port);

bool uart_port_dma_tx_busy(uint8_t port);

#endif
