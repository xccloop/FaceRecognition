/**
 * uart_port.c — STM32F103 DMA port (adapted from HAL to SPL)
 *
 * Original: Huawei's DMA-RingBuffer UART Framework
 * Adapted for: FaceRecognition project (SPL + FreeRTOS + NVIC_PriorityGroup_4)
 *
 * USART → DMA channel mapping (STM32F103):
 *   USART1_TX=DMA1_CH4  RX=DMA1_CH5    (APB2 72MHz)
 *   USART2_TX=DMA1_CH7  RX=DMA1_CH6    (APB1 36MHz)
 *   USART3_TX=DMA1_CH2  RX=DMA1_CH3    (APB1 36MHz)
 *
 * Key design decision: USART RX is NOT enabled in init() — it is deferred
 * to dma_rx_start().  This prevents spurious bytes captured by the USART
 * receiver before DMA is configured, which would otherwise be stored in DR
 * and transferred by DMA once it starts.
 */
#include "uart_port.h"
#include "stm32f10x.h"
#include <string.h>

/* ── SPL per-channel DMA CCR aliases → generic names ── */
#define DMA_CCR_EN     ((uint16_t)0x0001)
#define DMA_CCR_TCIE   ((uint16_t)0x0002)
#define DMA_CCR_HTIE   ((uint16_t)0x0004)
#define DMA_CCR_DIR    ((uint16_t)0x0010)
#define DMA_CCR_CIRC   ((uint16_t)0x0020)
#define DMA_CCR_MINC   ((uint16_t)0x0080)
#define DMA_CCR_PL     ((uint16_t)0x3000)
#define DMA_CCR_PL_0   ((uint16_t)0x1000)
#define DMA_CCR_PL_1   ((uint16_t)0x2000)

/* ── NVIC direct register access ── */
#define NVIC_ISER_BASE  0xE000E100UL
#define NVIC_ICER_BASE  0xE000E180UL
#define NVIC_IP_BASE    0xE000E400UL

#define IRQ_PRIO        13

static void nvic_enable(IRQn_Type irq)
{
    *(volatile uint32_t *)(NVIC_ISER_BASE + (((uint32_t)irq >> 5) << 2)) =
        (1UL << ((uint32_t)irq & 0x1F));
}
static void nvic_disable(IRQn_Type irq)
{
    *(volatile uint32_t *)(NVIC_ICER_BASE + (((uint32_t)irq >> 5) << 2)) =
        (1UL << ((uint32_t)irq & 0x1F));
}
static void nvic_set_prio(IRQn_Type irq, uint8_t prio)
{
    *(volatile uint8_t *)(NVIC_IP_BASE + (uint32_t)irq) = (uint8_t)(prio << 4);
}

/* ── Port state ── */
typedef struct {
    USART_TypeDef       *usart;
    DMA_Channel_TypeDef *dma_tx_ch, *dma_rx_ch;
    IRQn_Type            usart_irq, dma_tx_irq, dma_rx_irq;
    port_tx_tc_cb_t      tx_tc_cb;
    port_tx_ht_cb_t      tx_ht_cb;
    port_rx_tc_cb_t      rx_tc_cb;
    port_rx_ht_cb_t      rx_ht_cb;
    port_idle_cb_t       idle_cb;
    port_error_cb_t      error_cb;
    volatile bool        tx_busy;
    uint8_t             *rx_buf;
    uint32_t             rx_len;
} port_t;

static port_t g_port[UART_PORT_MAX];

/* ── 全局 IDLE 计数器，由 ISR 递增，任务端读取 ── */
volatile uint32_t g_idle_count = 0;

/* ── Per-port GPIO/Clock init ── */
static void msp_init(uint8_t p)
{
    volatile uint32_t tmp;
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    tmp = RCC->AHBENR; (void)tmp;
    switch (p) {
    case UART_PORT1:
        RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPAEN;
        tmp = RCC->APB2ENR; (void)tmp;
        /* PA9 = USART1_TX: alternate push-pull, 50MHz */
        GPIOA->CRH = (GPIOA->CRH & ~(GPIO_CRH_CNF9 | GPIO_CRH_MODE9))
                   | GPIO_CRH_CNF9_1 | GPIO_CRH_MODE9_0 | GPIO_CRH_MODE9_1;
        /* PA10 = USART1_RX: input floating */
        GPIOA->CRH = (GPIOA->CRH & ~(GPIO_CRH_CNF10 | GPIO_CRH_MODE10))
                   | GPIO_CRH_CNF10_0;
        break;
    case UART_PORT2:
        RCC->APB1ENR |= RCC_APB1ENR_USART2EN; RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
        tmp = RCC->APB2ENR; (void)tmp;
        GPIOA->CRL = (GPIOA->CRL & ~(GPIO_CRL_CNF2 | GPIO_CRL_MODE2))
                   | GPIO_CRL_CNF2_1 | GPIO_CRL_MODE2_0 | GPIO_CRL_MODE2_1;
        GPIOA->CRL = (GPIOA->CRL & ~(GPIO_CRL_CNF3 | GPIO_CRL_MODE3))
                   | GPIO_CRL_CNF3_0;
        break;
    case UART_PORT3:
        RCC->APB1ENR |= RCC_APB1ENR_USART3EN; RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
        tmp = RCC->APB2ENR; (void)tmp;
        GPIOB->CRH = (GPIOB->CRH & ~(GPIO_CRH_CNF10 | GPIO_CRH_MODE10))
                   | GPIO_CRH_CNF10_1 | GPIO_CRH_MODE10_0 | GPIO_CRH_MODE10_1;
        GPIOB->CRH = (GPIOB->CRH & ~(GPIO_CRH_CNF11 | GPIO_CRH_MODE11))
                   | GPIO_CRH_CNF11_0;
        break;
    }
}

/* ── Init ── */
void uart_port_init(uint8_t p, uint32_t baud)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    memset(q, 0, sizeof(*q));

    switch (p) {
    case UART_PORT1:
        q->usart = USART1; q->dma_tx_ch = DMA1_Channel4; q->dma_rx_ch = DMA1_Channel5;
        q->usart_irq = USART1_IRQn; q->dma_tx_irq = DMA1_Channel4_IRQn; q->dma_rx_irq = DMA1_Channel5_IRQn;
        break;
    case UART_PORT2:
        q->usart = USART2; q->dma_tx_ch = DMA1_Channel7; q->dma_rx_ch = DMA1_Channel6;
        q->usart_irq = USART2_IRQn; q->dma_tx_irq = DMA1_Channel7_IRQn; q->dma_rx_irq = DMA1_Channel6_IRQn;
        break;
    case UART_PORT3:
        q->usart = USART3; q->dma_tx_ch = DMA1_Channel2; q->dma_rx_ch = DMA1_Channel3;
        q->usart_irq = USART3_IRQn; q->dma_tx_irq = DMA1_Channel2_IRQn; q->dma_rx_irq = DMA1_Channel3_IRQn;
        break;
    }

    msp_init(p);

    extern uint32_t SystemCoreClock;
    uint32_t pclk = (p == UART_PORT1) ? SystemCoreClock : (SystemCoreClock / 2);
    q->usart->BRR = (pclk + baud / 2) / baud;

    /* TX + IDLE interrupt enabled; RX deferred to dma_rx_start() */
    q->usart->CR1 = USART_CR1_TE | USART_CR1_IDLEIE;
    q->usart->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;

    /* DMA TX: mem->periph, 8-bit, normal, TC interrupt */
    q->dma_tx_ch->CCR = 0;
    q->dma_tx_ch->CCR = DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_PL | DMA_CCR_TCIE;

    /* DMA RX: periph->mem, 8-bit, circular, TC+HT interrupts */
    q->dma_rx_ch->CCR = 0;
    q->dma_rx_ch->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PL_1 | DMA_CCR_TCIE | DMA_CCR_HTIE;

    /* NVIC */
    nvic_set_prio(q->usart_irq,   IRQ_PRIO);
    nvic_set_prio(q->dma_tx_irq,  IRQ_PRIO);
    nvic_set_prio(q->dma_rx_irq,  IRQ_PRIO);
    nvic_enable(q->usart_irq);

    q->usart->CR1 |= USART_CR1_UE;
}

void uart_port_deinit(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    q->dma_tx_ch->CCR &= ~DMA_CCR_EN;
    q->dma_rx_ch->CCR &= ~DMA_CCR_EN;
    q->usart->CR1 &= ~USART_CR1_UE;
    nvic_disable(q->usart_irq);
    nvic_disable(q->dma_tx_irq);
    nvic_disable(q->dma_rx_irq);
}

/* ── DMA TX start/stop ── */
void uart_port_dma_tx_start(uint8_t p, const uint8_t *data, uint32_t len)
{
    if (p >= UART_PORT_MAX || !data || len == 0) return;
    port_t *q = &g_port[p];
    q->dma_tx_ch->CCR &= ~DMA_CCR_EN;
    q->dma_tx_ch->CPAR  = (uint32_t)&q->usart->DR;
    q->dma_tx_ch->CMAR  = (uint32_t)data;
    q->dma_tx_ch->CNDTR = (uint16_t)len;
    q->tx_busy = true;
    nvic_enable(q->dma_tx_irq);
    q->dma_tx_ch->CCR |= DMA_CCR_EN;
}

void uart_port_dma_tx_stop(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    g_port[p].dma_tx_ch->CCR &= ~DMA_CCR_EN;
    nvic_disable(g_port[p].dma_tx_irq);
    g_port[p].tx_busy = false;
}

/* ── DMA RX start/stop ── */
void uart_port_dma_rx_start(uint8_t p, uint8_t *buf, uint32_t len)
{
    if (p >= UART_PORT_MAX || !buf || len == 0) return;
    port_t *q = &g_port[p];
    q->dma_rx_ch->CCR &= ~DMA_CCR_EN;
    q->dma_rx_ch->CPAR  = (uint32_t)&q->usart->DR;
    q->dma_rx_ch->CMAR  = (uint32_t)buf;
    q->dma_rx_ch->CNDTR = (uint16_t)len;
    q->rx_buf = buf; q->rx_len = len;
    nvic_enable(q->dma_rx_irq);
    q->dma_rx_ch->CCR |= DMA_CCR_EN;

    /* Now DMA is ready — enable USART RX and flush DR */
    q->usart->CR1 |= USART_CR1_RE;
    (void)q->usart->SR;
    (void)q->usart->DR;
}

void uart_port_dma_rx_stop(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    q->usart->CR1 &= ~USART_CR1_RE;
    q->dma_rx_ch->CCR &= ~DMA_CCR_EN;
    nvic_disable(q->dma_rx_irq);
}

/* ── DMA status ── */
uint32_t uart_port_tx_dma_remaining(uint8_t p) {
    return (p < UART_PORT_MAX) ? g_port[p].dma_tx_ch->CNDTR : 0;
}
uint32_t uart_port_rx_dma_remaining(uint8_t p) {
    return (p < UART_PORT_MAX) ? g_port[p].dma_rx_ch->CNDTR : 0;
}
bool uart_port_dma_tx_busy(uint8_t p) {
    return (p < UART_PORT_MAX) && g_port[p].tx_busy;
}

/* ── Callback setters ── */
void uart_port_set_tx_tc_cb(uint8_t p, port_tx_tc_cb_t cb)   { if (p < UART_PORT_MAX) g_port[p].tx_tc_cb = cb; }
void uart_port_set_tx_ht_cb(uint8_t p, port_tx_ht_cb_t cb)   { if (p < UART_PORT_MAX) g_port[p].tx_ht_cb = cb; }
void uart_port_set_rx_tc_cb(uint8_t p, port_rx_tc_cb_t cb)   { if (p < UART_PORT_MAX) g_port[p].rx_tc_cb = cb; }
void uart_port_set_rx_ht_cb(uint8_t p, port_rx_ht_cb_t cb)   { if (p < UART_PORT_MAX) g_port[p].rx_ht_cb = cb; }
void uart_port_set_idle_cb(uint8_t p, port_idle_cb_t cb)     { if (p < UART_PORT_MAX) g_port[p].idle_cb = cb; }
void uart_port_set_error_cb(uint8_t p, port_error_cb_t cb)   { if (p < UART_PORT_MAX) g_port[p].error_cb = cb; }

/* ── UART ISR (IDLE + error) — 不做任何 TX 操作，避免干扰 USART 状态 ── */
void uart_port_irq_handler(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    uint32_t sr = q->usart->SR;

    if ((sr & USART_SR_IDLE) && (q->usart->CR1 & USART_CR1_IDLEIE)) {
        volatile uint32_t dr = q->usart->DR;
        (void)dr;
        g_idle_count++;                     /* 极简诊断：只累加计数器 */
        if (q->idle_cb) q->idle_cb(p);
    }

    if (sr & (USART_SR_PE | USART_SR_FE | USART_SR_NE | USART_SR_ORE)) {
        volatile uint32_t dr = q->usart->DR;
        (void)dr;
        if (q->error_cb) q->error_cb(p, sr & 0x0F);
    }
}

/* ── DMA TX ISR ── */
void uart_port_dma_tx_irq_handler(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    static const uint8_t ch_idx[3] = {4, 7, 2};
    uint32_t tc = DMA_ISR_TCIF1 << ((ch_idx[p] - 1) * 4);

    if (DMA1->ISR & tc) {
        DMA1->IFCR = tc;
        q->tx_busy = false;
        nvic_disable(q->dma_tx_irq);
        if (q->tx_tc_cb) q->tx_tc_cb(p);
    }
}

/* ── DMA RX ISR ── */
void uart_port_dma_rx_irq_handler(uint8_t p)
{
    if (p >= UART_PORT_MAX) return;
    port_t *q = &g_port[p];
    static const uint8_t ch_idx[3] = {5, 6, 3};
    uint32_t tc = DMA_ISR_TCIF1 << ((ch_idx[p] - 1) * 4);
    uint32_t ht = DMA_ISR_HTIF1 << ((ch_idx[p] - 1) * 4);
    uint32_t isr = DMA1->ISR;

    if (isr & tc) {
        DMA1->IFCR = tc;
        if (q->rx_tc_cb) q->rx_tc_cb(p);
    }
    if (isr & ht) {
        DMA1->IFCR = ht;
        if (q->rx_ht_cb) q->rx_ht_cb(p);
    }
}
