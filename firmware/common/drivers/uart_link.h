/* UART link to the host (Design Document §8.1): USART3 TX on PD8, 230400 baud, 8N1, no flow
 * control, transmit only. MCU1 only.
 *
 * Latest wins (drivers/tx_slots.h): the caller writes each frame straight into
 * a slot buffer and hands it over. While the link is busy, only the newest frame of each kind
 * waits; an older one still waiting is replaced, never queued behind. The buffers belong to the
 * caller, which puts them in .noinit RAM (util/noinit.h).
 *
 * The mover, which feeds the bytes to the USART, is the USART3 interrupt, one byte per TXE.
 * On hardware it can be a DMA stream instead (Renode's USART cannot request a DMA transfer for
 * sending): the stream's transfer-complete interrupt calls tx_slots_done() and tx_slots_next()
 * and starts the next transfer, and uart_link_commit() sets that interrupt pending when the
 * stream is idle. Nothing outside this file changes.
 *
 * Writer functions are for the main loop only. */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/tx_slots.h"

void uart_link_init(void);

/* Gives a slot its two buffers (size bytes each). After uart_link_init(), before the first
 * uart_link_begin(). */
void uart_link_attach(tx_slot_t slot, uint8_t *buf_a, uint8_t *buf_b, uint32_t size);

/* The buffer for the next frame of this kind and its size; *replaced is true if a frame of this
 * kind that had not started yet was taken back for it. See tx_slots_begin(). */
uint8_t *uart_link_begin(tx_slot_t slot, uint32_t *size, bool *replaced);

/* Hands over the len bytes written since uart_link_begin() and returns at once. */
void uart_link_commit(tx_slot_t slot, uint32_t len);

/* The event queue (tx_slots.h): events are never replaced; a full queue refuses a new one. */
void uart_link_attach_queue(uint8_t *entries, uint32_t entry_size);
uint8_t *uart_link_queue_begin(uint32_t *size); /* NULL if the queue is full */
void uart_link_queue_commit(uint32_t len);

void uart_link_get_stats(tx_slots_stats_t *out);

#endif
