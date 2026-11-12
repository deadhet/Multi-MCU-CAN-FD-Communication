#include "stm32h743xx.h"
#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/uart_link.h"

#define LINK_BAUD     230400U /* Design Document §8.1 */
#define LINK_PRIORITY 3U      /* As the debug UART: below FDCAN and SysTick (§4) */

/* Mover state, USART3_IRQHandler() only. */
static const uint8_t *tx_data;
static uint32_t tx_left;

void uart_link_init(void)
{
    tx_slots_init();
    tx_data = 0;
    tx_left = 0U;

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

void uart_link_attach(tx_slot_t slot, uint8_t *buf_a, uint8_t *buf_b, uint32_t size)
{
    tx_slots_attach(slot, buf_a, buf_b, size);
}

uint8_t *uart_link_begin(tx_slot_t slot, uint32_t *size, bool *replaced)
{
    return tx_slots_begin(slot, size, replaced);
}

void uart_link_commit(tx_slot_t slot, uint32_t len)
{
    tx_slots_commit(slot, len);
    /* Wake the mover. If it is busy this changes nothing: it finds the frame when the current
     * one is done. If it has just gone quiet, it went quiet before the frame above was handed
     * over (an interrupt finishes before the main loop goes on), so this wakes it for it. */
    USART3->CR1 |= USART_CR1_TXEIE;
}

void uart_link_get_stats(tx_slots_stats_t *out)
{
    tx_slots_get_stats(out);
}

/* The mover. TXE: the data register is empty and takes the next byte. */
void USART3_IRQHandler(void)
{
    if ((tx_left == 0U) && !tx_slots_next(&tx_data, &tx_left)) {
        USART3->CR1 &= ~USART_CR1_TXEIE; /* Nothing pending: quiet until the next hand-over */
        return;
    }
    USART3->TDR = *tx_data;
    tx_data++;
    tx_left--;
    if (tx_left == 0U) {
        tx_slots_done(); /* The last byte is in the USART: the buffer may be written again */
    }
}
