/**
 * ringbuffer.h — SPSC lock-free ring buffer
 * ARMCC v5 兼容：手工 bool typedef（与 uart_port.h 共享 _STDBOOL_COMPAT_ guard）
 */
#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "stm32f10x.h"

#ifndef _STDBOOL_COMPAT_
#define _STDBOOL_COMPAT_
typedef unsigned char bool;
#define false 0
#define true  1
#endif

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint8_t  *buffer;
    uint32_t  mask;
    uint32_t  capacity;
} ringbuffer_t;

void     rb_init(ringbuffer_t *rb, uint8_t *buf, uint32_t capacity);
uint32_t rb_put(ringbuffer_t *rb, const uint8_t *data, uint32_t len);
uint32_t rb_get(ringbuffer_t *rb, uint8_t *dst, uint32_t len);
uint32_t rb_peek(const ringbuffer_t *rb, uint8_t *dst, uint32_t len);
void     rb_discard(ringbuffer_t *rb, uint32_t len);
uint32_t rb_available(const ringbuffer_t *rb);
uint32_t rb_free_space(const ringbuffer_t *rb);
bool     rb_is_empty(const ringbuffer_t *rb);
bool     rb_is_full(const ringbuffer_t *rb);
void     rb_flush(ringbuffer_t *rb);
void     rb_reset(ringbuffer_t *rb);
uint32_t rb_contig_read_len(const ringbuffer_t *rb);
uint32_t rb_contig_write_len(const ringbuffer_t *rb);
uint32_t rb_get_head(const ringbuffer_t *rb);
uint32_t rb_get_tail(const ringbuffer_t *rb);
void     rb_advance_head(ringbuffer_t *rb, uint32_t len);
void     rb_advance_tail(ringbuffer_t *rb, uint32_t len);

#endif
