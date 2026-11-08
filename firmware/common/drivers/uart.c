#include "stm32h743xx.h"
#include "drivers/clock.h"
#include "drivers/uart.h"

#define DEBUG_BAUD 115200U
#define UART_PRIORITY 3U

#define TX_RING_SIZE 2048U /* Must be a power of two */
#define TX_RING_MASK (TX_RING_SIZE - 1U)

static uint8_t tx_ring[TX_RING_SIZE];
static volatile uint32_t tx_head; /* Written only by uart_debug_write() */
static volatile uint32_t tx_tail; /* Written only by USART1_IRQHandler() */
static volatile uint32_t tx_dropped;

void uart_debug_init(void)
{
    USART1->CR1 = 0U; /* RM0433 p.2082, p.2099: BRR and PRESC are writable only while UE = 0. */
    USART1->PRESC = 0U; /* RM0433 p.2099: PRESCALER = 0000, clock not divided. Write while UE = 0. */
    
    // DEBUG_BAUD/2U is added to make the nearest Round off instead of just the FLOOR.
    USART1->BRR = (CLOCK_PCLK_HZ + (DEBUG_BAUD / 2U)) / DEBUG_BAUD;
    /* RM0433 p.2025: enable the USART first, then the transmitter. */
    USART1->CR1 = USART_CR1_UE;
    USART1->CR1 |= USART_CR1_TE;

    NVIC_SetPriority(USART1_IRQn, UART_PRIORITY);
    NVIC_EnableIRQ(USART1_IRQn);
}

void uart_debug_write(const char *text)
{
    uint32_t len = 0U;
    while (text[len] != '\0') {
        len++;
    }

    uint32_t head = tx_head;
    // this makes result MOD of 2048, one slot always stays empty signifying head == tail means empty.
    uint32_t free_bytes = (tx_tail - head - 1U) & TX_RING_MASK;
    if (len > free_bytes) {
        tx_dropped++;
        return;
    }

    for (uint32_t i = 0U; i < len; i++) {
        tx_ring[head] = (uint8_t)text[i];
        head = (head + 1U) & TX_RING_MASK;
    }
    // this line ensures that once the data is into the ring, it only starts to send data then.
    __DMB(); /* Data must be in the ring before the ISR can see the new head. */
    tx_head = head;

    USART1->CR1 |= USART_CR1_TXEIE;
}

//converts the 32bit integer to string values of each digit.
void uart_debug_write_u32(uint32_t value)
{
    char text[11]; /* 10 digits for 2^32 - 1, plus the terminator */
    uint32_t pos = sizeof(text) - 1U;

    text[pos] = '\0';
    do {
        pos--;
        text[pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    uart_debug_write(&text[pos]);
}

uint32_t uart_debug_dropped(void)
{
    return tx_dropped;
}

void USART1_IRQHandler(void)
{
    uint32_t tail = tx_tail;

    if (tail == tx_head) {
        USART1->CR1 &= ~USART_CR1_TXEIE;
        return;
    }

    uint8_t byte = tx_ring[tail];
    tx_tail = (tail + 1U) & TX_RING_MASK;
    USART1->TDR = byte;
}
