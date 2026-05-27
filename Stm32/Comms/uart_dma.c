/**
 * uart_dma.c — DMA-UART driver implementation
 */
#include "uart_dma.h"
#include "ringbuffer.h"
#include "uart_port.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

struct uart_dma_handle {
    uint8_t      port;
    ringbuffer_t tx_rb, rx_rb;
    TaskHandle_t tx_task, rx_task;
    volatile uint32_t kick_head;
    volatile bool     chain_pending;
    volatile bool     idle;
    uint32_t      rx_prev_cndtr;   /* for delta-based rx_update */
    volatile uint32_t error_count;    /* ORE/NE/FE errors since boot */
    volatile uint32_t error_restarts; /* DMA RX restarts after error */
};

static uart_dma_handle_t *g_handle[UART_PORT_MAX];

/* ── forward ISR callbacks ── */
static void on_tx_tc(uint8_t p);
static void on_rx_ht(uint8_t p);
static void on_rx_tc(uint8_t p);
static void on_idle(uint8_t p);
static void on_error(uint8_t p, uint32_t e);

/* ── internal: start TX DMA from ring buffer ── */
static void tx_kick(uart_dma_handle_t *h)
{
    uint32_t head = rb_get_head(&h->tx_rb);
    uint32_t tail = rb_get_tail(&h->tx_rb);
    if (head == tail) { h->idle = true; return; }
    h->kick_head = head;
    h->idle = false;

    uint32_t ti = tail & h->tx_rb.mask;
    uint32_t hi = head & h->tx_rb.mask;

    if (tail < head) {
        /* no wrap */
        h->chain_pending = false;
        uart_port_dma_tx_start(h->port, &h->tx_rb.buffer[ti], head - tail);
    } else {
        /* wrap: [tail, capacity) + [0, head) */
        uint32_t seg1 = h->tx_rb.capacity - ti;
        h->chain_pending = (hi > 0);
        uart_port_dma_tx_start(h->port, &h->tx_rb.buffer[ti], seg1);
    }
}

/* ── public ── */
uart_dma_handle_t *uart_dma_open(uint8_t port, uint32_t baud,
    uint8_t *tx_buf, uint32_t tx_sz, uint8_t *rx_buf, uint32_t rx_sz)
{
    if (port >= UART_PORT_MAX) return NULL;
    uart_dma_handle_t *h = pvPortMalloc(sizeof(*h));
    if (!h) return NULL;
    h->port = port;
    rb_init(&h->tx_rb, tx_buf, tx_sz);
    rb_init(&h->rx_rb, rx_buf, rx_sz);
    h->tx_task = NULL;    /* registered later via uart_dma_set_rx_task() */
    h->rx_task = NULL;    /* scheduler not running yet, xTaskGetCurrentTaskHandle() unreliable */
    h->kick_head = 0; h->chain_pending = false; h->idle = true;
    h->error_count = 0; h->error_restarts = 0;

    uart_port_init(port, baud);               /* must come first: memset inside */
    uart_port_set_tx_tc_cb(port, on_tx_tc);   /* callbacks AFTER init (memset clears) */
    uart_port_set_rx_ht_cb(port, on_rx_ht);
    uart_port_set_rx_tc_cb(port, on_rx_tc);
    uart_port_set_idle_cb(port, on_idle);
    uart_port_set_error_cb(port, on_error);

    uart_port_dma_rx_start(port, rx_buf, rx_sz);
    h->rx_prev_cndtr = rx_sz;   /* initial CNDTR = rx_buf size */

    g_handle[port] = h;
    return h;
}

void uart_dma_close(uart_dma_handle_t *h)
{
    if (!h) return;
    uart_port_dma_tx_stop(h->port);
    uart_port_dma_rx_stop(h->port);
    uart_port_deinit(h->port);
    g_handle[h->port] = NULL;
    vPortFree(h);
}

uint32_t uart_dma_send(uart_dma_handle_t *h, const uint8_t *data, uint32_t len)
{
    if (!h || !data || len == 0) return 0;
    uint32_t n = rb_put(&h->tx_rb, data, len);
    if (h->idle && !rb_is_empty(&h->tx_rb)) tx_kick(h);
    return n;
}

uint32_t uart_dma_recv(uart_dma_handle_t *h, uint8_t *buf, uint32_t len)
{
    if (!h || !buf || len == 0) return 0;
    return rb_get(&h->rx_rb, buf, len);
}

uint32_t uart_dma_rx_available(uart_dma_handle_t *h) { return h ? rb_available(&h->rx_rb) : 0; }
uint32_t uart_dma_tx_free(uart_dma_handle_t *h)      { return h ? rb_free_space(&h->tx_rb) : 0; }

void uart_dma_rx_flush(uart_dma_handle_t *h) { if (h) rb_flush(&h->rx_rb); }

uint32_t uart_dma_error_count(uart_dma_handle_t *h)   { return h ? h->error_count : 0; }
uint32_t uart_dma_error_restarts(uart_dma_handle_t *h) { return h ? h->error_restarts : 0; }

bool uart_dma_tx_flush(uart_dma_handle_t *h, uint32_t to)
{
    if (!h) return true;
    TickType_t st = xTaskGetTickCount();
    while (!rb_is_empty(&h->tx_rb) || !h->idle) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        if (to != portMAX_DELAY && (xTaskGetTickCount() - st >= to)) return false;
    }
    return true;
}

bool uart_dma_tx_busy(uart_dma_handle_t *h) { return h && !h->idle; }
void uart_dma_set_rx_task(uart_dma_handle_t *h, void *task) { if (h) h->rx_task = (TaskHandle_t)task; }

/* ── ISR: TX TC ── */
static void on_tx_tc(uint8_t p)
{
    uart_dma_handle_t *h = g_handle[p]; if (!h) return;
    BaseType_t woken = pdFALSE;

    if (h->chain_pending) {
        h->chain_pending = false;
        uint32_t end = h->tx_rb.capacity;
        h->tx_rb.tail = (h->tx_rb.tail & ~h->tx_rb.mask) + end;
        if (h->tx_rb.tail > h->kick_head) h->tx_rb.tail = h->kick_head;
        uint32_t s2 = h->kick_head & h->tx_rb.mask;
        if (s2 > 0) { uart_port_dma_tx_start(p, &h->tx_rb.buffer[0], s2); return; }
    }

    h->tx_rb.tail = h->kick_head;
    if (rb_get_head(&h->tx_rb) != rb_get_tail(&h->tx_rb)) {
        tx_kick(h);
    } else {
        h->idle = true;
        if (h->tx_task) vTaskNotifyGiveFromISR(h->tx_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ── ISR: RX HT/TC ── */
static void rx_update(uart_dma_handle_t *h)
{
    uint32_t curr = uart_port_rx_dma_remaining(h->port);
    uint32_t prev = h->rx_prev_cndtr;
    uint32_t delta;

    if (curr <= prev) {
        delta = prev - curr;
    } else {
        /* DMA CNDTR reloaded (circular wrap) */
        delta = prev + (h->rx_rb.capacity - curr);
    }
    h->rx_prev_cndtr = curr;

    if (delta > 0) rb_advance_head(&h->rx_rb, delta);
}

static void on_rx_ht(uint8_t p) { rx_update(g_handle[p]); }
static void on_rx_tc(uint8_t p) { rx_update(g_handle[p]); }

/* ── ISR: IDLE ── */
static void on_idle(uint8_t p)
{
    uart_dma_handle_t *h = g_handle[p]; if (!h) return;
    rx_update(h);
    BaseType_t woken = pdFALSE;
    if (h->rx_task) vTaskNotifyGiveFromISR(h->rx_task, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── ISR: ERROR ──
   ORE/NE/FE flags are already cleared by uart_port_irq_handler (SR+DR read).
   We restart DMA RX because data received during the error window may be
   corrupted — discarding the ring buffer ensures the frame parser sees a
   clean stream on the next IDLE interrupt. */
static void on_error(uint8_t p, uint32_t e)
{
    uart_dma_handle_t *h = g_handle[p];
    if (!h) return;

    h->error_count++;
    h->error_restarts++;

    /* Flush RX ring buffer: data during error window is unreliable */
    rb_flush(&h->rx_rb);
    h->rx_prev_cndtr = h->rx_rb.capacity;

    /* Restart DMA RX from a clean state */
    uart_port_dma_rx_stop(p);
    uart_port_dma_rx_start(p, h->rx_rb.buffer, h->rx_rb.capacity);

    (void)e; /* error flags logged via counters, not individually dispatched */
}
