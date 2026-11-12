#include "stm32h743xx.h"
#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/uart_link.h"

#define LINK_BAUD     230400U /* Design Document §8.1 */
#define LINK_PRIORITY 3U      /* As the debug UART: below FDCAN and SysTick (§4) */

/* Power of two. Holds a worst-case snapshot and a CONFIG at the same time. */
#define TX_RING_SIZE 4096U
#define TX_RING_MASK (TX_RING_SIZE - 1U)

static uint8_t tx_ring[TX_RING_SIZE];
static volatile uint32_t tx_head; /* Written only by uart_link_send() */
static volatile uint32_t tx_tail; /* Written only by USART3_IRQHandler() */
static uart_link_stats_t stats;

void uart_link_init(void)
{
    board_uart_link_pins_init();

    /* As USART1: BRR and PRESC are writable only while UE = 0. 8 data bits, no parity and
     * 1 stop bit are the reset values of CR1 and CR2. */
    USART3->CR1 = 0U;
    USART3->PRESC = 0U;
    USART3->BRR = (CLOCK_PCLK_HZ + (LINK_BAUD / 2U)) / LINK_BAUD; /* 521: -0.03 % */
    USART3->CR1 = USART_CR1_UE;
    USART3->CR1 |= USART_CR1_TE;

    NVIC_SetPriority(USART3_IRQn, LINK_PRIORITY);
    NVIC_EnableIRQ(USART3_IRQn);
}

uint32_t uart_link_free(void)
{
    /* One slot always stays empty, so head == tail means empty. */
    return (tx_tail - tx_head - 1U) & TX_RING_MASK;
}

bool uart_link_send(const uint8_t *data, uint32_t len)
{
    uint32_t head = tx_head;

    if (len > ((tx_tail - head - 1U) & TX_RING_MASK)) {
        stats.refused++;
        return false;
    }
    for (uint32_t i = 0U; i < len; i++) {
        tx_ring[head] = data[i];
        head = (head + 1U) & TX_RING_MASK;
    }
    __DMB(); /* The bytes must be in the ring before the interrupt can see the new head. */
    tx_head = head;
    stats.frames++;
    stats.bytes += len;

    USART3->CR1 |= USART_CR1_TXEIE;
    return true;
}

void uart_link_get_stats(uart_link_stats_t *out)
{
    *out = stats;
}

void USART3_IRQHandler(void)
{
    uint32_t tail = tx_tail;

    if (tail == tx_head) {
        USART3->CR1 &= ~USART_CR1_TXEIE;
        return;
    }
    uint8_t byte = tx_ring[tail];
    tx_tail = (tail + 1U) & TX_RING_MASK;
    USART3->TDR = byte;
}
