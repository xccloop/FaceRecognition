/**
 * uart_api.c — UART API implementation (mutex-protected, blocking/async)
 *
 * Adapted from: Huawei's DMA-RingBuffer UART Framework
 * Adapted for:  FaceRecognition STM32F103 (SPL + ARMCC v5)
 *
 * Provides: uart_init/deinit, uart_send/recv (blocking + timeout),
 *           uart_send_async/recv_async, uart_printf, uart_yield (frame callback)
 */
#include "uart_api.h"
#include "uart_dma.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ── Per-port context ── */
typedef struct {
    uart_dma_handle_t *drv;
    SemaphoreHandle_t  tx_mutex;
    uart_rx_cb_t       rx_cb;
    void              *rx_cb_arg;
    uint8_t           *tx_storage, *rx_storage;
    bool               init;
} ctx_t;

static ctx_t g_ctx[UART_PORT_MAX];

/* ── Init / Deinit ── */
void uart_init(uint8_t port, uint32_t baud, uint32_t tx_sz, uint32_t rx_sz)
{
    if (port >= UART_PORT_MAX) return;
    ctx_t *c = &g_ctx[port];
    if (c->init) uart_deinit(port);
    memset(c, 0, sizeof(*c));

    c->tx_storage = pvPortMalloc(tx_sz);
    c->rx_storage = pvPortMalloc(rx_sz);
    c->tx_mutex   = xSemaphoreCreateMutex();
    if (!c->tx_storage || !c->rx_storage || !c->tx_mutex) return;

    c->drv = uart_dma_open(port, baud, c->tx_storage, tx_sz, c->rx_storage, rx_sz);
    if (!c->drv) return;
    c->init = true;
}

void uart_deinit(uint8_t port)
{
    if (port >= UART_PORT_MAX) return;
    ctx_t *c = &g_ctx[port];
    if (!c->init) return;
    if (c->drv)       { uart_dma_close(c->drv); }
    if (c->tx_mutex)  { vSemaphoreDelete(c->tx_mutex); }
    if (c->tx_storage) { vPortFree(c->tx_storage); }
    if (c->rx_storage) { vPortFree(c->rx_storage); }
    c->init = false;
}

/* ── Blocking send (mutex-protected, retries when buffer full) ── */
int32_t uart_send(uint8_t port, const uint8_t *data, uint32_t len, uint32_t timeout)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init || !data || len == 0) return -1;
    if (xSemaphoreTake(c->tx_mutex, timeout) != pdTRUE) return -1;

    uint32_t sent = 0;
    TickType_t st = xTaskGetTickCount();
    while (sent < len) {
        uint32_t f = uart_dma_tx_free(c->drv);
        if (f == 0) {
            if (timeout != portMAX_DELAY && (xTaskGetTickCount() - st >= timeout)) break;
            xSemaphoreGive(c->tx_mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
            if (xSemaphoreTake(c->tx_mutex, timeout) != pdTRUE) return (int32_t)sent;
            continue;
        }
        uint32_t ch = len - sent;
        if (ch > f) ch = f;
        sent += uart_dma_send(c->drv, data + sent, ch);
    }
    xSemaphoreGive(c->tx_mutex);
    return (int32_t)sent;
}

/* ── Blocking recv (task-notify driven, timeout-aware) ── */
int32_t uart_recv(uint8_t port, uint8_t *buf, uint32_t len, uint32_t timeout)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init || !buf || len == 0) return -1;
    uart_dma_set_rx_task(c->drv, xTaskGetCurrentTaskHandle());

    TickType_t st = xTaskGetTickCount();
    while (1) {
        uint32_t a = uart_dma_rx_available(c->drv);
        if (a > 0) {
            if (len > a) len = a;
            return (int32_t)uart_dma_recv(c->drv, buf, len);
        }
        uint32_t w = pdMS_TO_TICKS(10);
        if (timeout != portMAX_DELAY) {
            TickType_t el = xTaskGetTickCount() - st;
            if (el >= timeout) return 0;
            if (w > timeout - el) w = timeout - el;
        }
        ulTaskNotifyTake(pdTRUE, w);
    }
}

/* ── Non-blocking send/receive ── */
int32_t uart_send_async(uint8_t port, const uint8_t *data, uint32_t len)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init || !data || len == 0) return -1;
    if (xSemaphoreTake(c->tx_mutex, 0) != pdTRUE) return -1;
    uint32_t f = uart_dma_tx_free(c->drv);
    if (f == 0) { xSemaphoreGive(c->tx_mutex); return -1; }
    if (len > f) len = f;
    uint32_t n = uart_dma_send(c->drv, data, len);
    xSemaphoreGive(c->tx_mutex);
    return (int32_t)n;
}

int32_t uart_recv_async(uint8_t port, uint8_t *buf, uint32_t len)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init || !buf || len == 0) return -1;
    return (int32_t)uart_dma_recv(c->drv, buf, len);
}

/* ── Flush ── */
bool uart_tx_flush(uint8_t port, uint32_t timeout)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init) return true;
    if (xSemaphoreTake(c->tx_mutex, timeout) != pdTRUE) return false;
    bool r = uart_dma_tx_flush(c->drv, timeout);
    xSemaphoreGive(c->tx_mutex);
    return r;
}

void uart_rx_flush(uint8_t port) {
    if (port < UART_PORT_MAX && g_ctx[port].init) uart_dma_rx_flush(g_ctx[port].drv);
}

/* ── printf ── */
int32_t uart_printf(uint8_t port, const char *fmt, ...)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init || !fmt) return -1;
    char buf[UART_PRINTF_BUF];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return -1;
    if ((uint32_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    return uart_send(port, (const uint8_t *)buf, (uint32_t)n, portMAX_DELAY);
}

/* ── Status ── */
uint32_t uart_rx_available(uint8_t port) { return (port < UART_PORT_MAX && g_ctx[port].init) ? uart_dma_rx_available(g_ctx[port].drv) : 0; }
uint32_t uart_tx_free(uint8_t port)      { return (port < UART_PORT_MAX && g_ctx[port].init) ? uart_dma_tx_free(g_ctx[port].drv) : 0; }

/* ── Frame callback (driven by IDLE interrupt) ── */
void uart_set_rx_callback(uint8_t port, uart_rx_cb_t cb, void *arg) {
    if (port < UART_PORT_MAX) { g_ctx[port].rx_cb = cb; g_ctx[port].rx_cb_arg = arg; }
}

void uart_yield(uint8_t port, uint32_t timeout)
{
    ctx_t *c = &g_ctx[port];
    if (!c->init) return;
    uart_dma_set_rx_task(c->drv, xTaskGetCurrentTaskHandle());
    TickType_t st = xTaskGetTickCount();
    while (1) {
        uint32_t a = uart_dma_rx_available(c->drv);
        if (a > 0 && c->rx_cb) {
            uint8_t f[256];
            uint32_t n = a < 256 ? a : 256;
            uart_dma_recv(c->drv, f, n);
            c->rx_cb(port, f, n, c->rx_cb_arg);
        }
        uint32_t w = pdMS_TO_TICKS(100);
        if (timeout != portMAX_DELAY) {
            TickType_t el = xTaskGetTickCount() - st;
            if (el >= timeout) break;
            if (w > timeout - el) w = timeout - el;
        }
        ulTaskNotifyTake(pdTRUE, w);
    }
}
