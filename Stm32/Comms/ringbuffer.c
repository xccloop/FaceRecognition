#include "ringbuffer.h"
#include <string.h>

/* forward declarations (ARMCC v5 requires declaration before use) */
uint32_t rb_free_space(const ringbuffer_t *rb);
uint32_t rb_available(const ringbuffer_t *rb);

static bool is_power_of_two(uint32_t x) { return (x > 0) && ((x & (x - 1)) == 0); }

void rb_init(ringbuffer_t *rb, uint8_t *buf, uint32_t capacity)
{
    if (!rb || !buf || !is_power_of_two(capacity)) return;
    rb->buffer = buf; rb->capacity = capacity;
    rb->mask = capacity - 1; rb->head = 0; rb->tail = 0;
}

uint32_t rb_put(ringbuffer_t *rb, const uint8_t *data, uint32_t len)
{
    if (!rb || !data || len == 0) return 0;
    uint32_t free = rb_free_space(rb);
    if (len > free) len = free;
    if (len == 0) return 0;

    uint32_t idx = rb->head & rb->mask;
    uint32_t seg1 = rb->capacity - idx;
    if (seg1 >= len) {
        memcpy(&rb->buffer[idx], data, len);
    } else {
        memcpy(&rb->buffer[idx], data, seg1);
        memcpy(&rb->buffer[0], data + seg1, len - seg1);
    }
    rb->head += len;
    return len;
}

uint32_t rb_get(ringbuffer_t *rb, uint8_t *dst, uint32_t len)
{
    if (!rb || !dst || len == 0) return 0;
    uint32_t avail = rb_available(rb);
    if (len > avail) len = avail;
    if (len == 0) return 0;

    uint32_t idx = rb->tail & rb->mask;
    uint32_t seg1 = rb->capacity - idx;
    if (seg1 >= len) {
        memcpy(dst, &rb->buffer[idx], len);
    } else {
        memcpy(dst, &rb->buffer[idx], seg1);
        memcpy(dst + seg1, &rb->buffer[0], len - seg1);
    }
    rb->tail += len;
    return len;
}

uint32_t rb_peek(const ringbuffer_t *rb, uint8_t *dst, uint32_t len)
{
    if (!rb || !dst || len == 0) return 0;
    uint32_t avail = rb_available(rb);
    if (len > avail) len = avail;
    if (len == 0) return 0;
    uint32_t idx = rb->tail & rb->mask;
    uint32_t seg1 = rb->capacity - idx;
    if (seg1 >= len) {
        memcpy(dst, &rb->buffer[idx], len);
    } else {
        memcpy(dst, &rb->buffer[idx], seg1);
        memcpy(dst + seg1, &rb->buffer[0], len - seg1);
    }
    return len;
}

void rb_discard(ringbuffer_t *rb, uint32_t len) {
    if (!rb) return;
    uint32_t avail = rb_available(rb);
    if (len > avail) len = avail;
    rb->tail += len;
}

uint32_t rb_available(const ringbuffer_t *rb)  { return rb ? rb->head - rb->tail : 0; }
uint32_t rb_free_space(const ringbuffer_t *rb) { return rb ? rb->capacity - (rb->head - rb->tail) : 0; }
bool rb_is_empty(const ringbuffer_t *rb)       { return rb ? rb->head == rb->tail : true; }
bool rb_is_full(const ringbuffer_t *rb)        { return rb ? (rb->head - rb->tail) >= rb->capacity : false; }
void rb_flush(ringbuffer_t *rb)                { if (rb) { rb->head = 0; rb->tail = 0; } }
void rb_reset(ringbuffer_t *rb)                { if (rb) { rb->head = 0; rb->tail = 0; } }

uint32_t rb_get_head(const ringbuffer_t *rb)   { return rb ? rb->head : 0; }
uint32_t rb_get_tail(const ringbuffer_t *rb)   { return rb ? rb->tail : 0; }

uint32_t rb_contig_read_len(const ringbuffer_t *rb) {
    if (!rb) return 0;
    uint32_t avail = rb_available(rb);
    uint32_t seg1  = rb->capacity - (rb->tail & rb->mask);
    return (avail < seg1) ? avail : seg1;
}

uint32_t rb_contig_write_len(const ringbuffer_t *rb) {
    if (!rb) return 0;
    uint32_t free = rb_free_space(rb);
    uint32_t seg1 = rb->capacity - (rb->head & rb->mask);
    return (free < seg1) ? free : seg1;
}

void rb_advance_head(ringbuffer_t *rb, uint32_t len) {
    if (!rb || len == 0) return;
    uint32_t free = rb_free_space(rb);
    if (len > free) { rb->tail += (len - free); }
    rb->head += len;
}

void rb_advance_tail(ringbuffer_t *rb, uint32_t len) {
    if (!rb || len == 0) return;
    uint32_t avail = rb_available(rb);
    if (len > avail) len = avail;
    rb->tail += len;
}
