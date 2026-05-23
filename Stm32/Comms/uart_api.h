/**
 * uart_api.h — High-level UART API (FreeRTOS thread-safe)
 *
 * Adapted from: Huawei's DMA-RingBuffer UART Framework
 * Adapted for:  FaceRecognition STM32F103 (SPL + ARMCC v5)
 */
#ifndef UART_API_H
#define UART_API_H

#include "uart_port.h"   /* for UART_PORT_MAX, _STDBOOL_COMPAT_ */

#if defined(_STDBOOL_COMPAT_) && !defined(__cplusplus)
typedef unsigned char bool;
#define true  1
#define false 0
#endif

#include <stdint.h>

#define UART_PRINTF_BUF     256

/* Init / Deinit */
void uart_init(uint8_t port, uint32_t baud, uint32_t tx_sz, uint32_t rx_sz);
void uart_deinit(uint8_t port);

/* Blocking (with timeout, ticks) */
int32_t uart_send(uint8_t port, const uint8_t *data, uint32_t len, uint32_t timeout);
int32_t uart_recv(uint8_t port, uint8_t *buf, uint32_t len, uint32_t timeout);
bool    uart_tx_flush(uint8_t port, uint32_t timeout);

/* Non-blocking */
int32_t uart_send_async(uint8_t port, const uint8_t *data, uint32_t len);
int32_t uart_recv_async(uint8_t port, uint8_t *buf, uint32_t len);

/* Formatted output */
int32_t uart_printf(uint8_t port, const char *fmt, ...);

/* Status */
void     uart_rx_flush(uint8_t port);
uint32_t uart_rx_available(uint8_t port);
uint32_t uart_tx_free(uint8_t port);

/* Frame callback (IDLE-driven) */
typedef void (*uart_rx_cb_t)(uint8_t port, const uint8_t *data, uint32_t len, void *arg);
void uart_set_rx_callback(uint8_t port, uart_rx_cb_t cb, void *arg);
void uart_yield(uint8_t port, uint32_t timeout);

#endif
