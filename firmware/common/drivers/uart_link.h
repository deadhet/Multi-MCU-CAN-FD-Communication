/* UART link to the host (Design Document §8.1): USART3 TX on PD8, 230400 baud, 8N1, no flow
 * control, transmit only. MCU1 only. An interrupt sends the bytes from a 4 KB ring. The ring takes
 * whole frames only: a frame either fits completely or is refused, so the host never receives
 * part of a frame. Called from the main loop only. */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t frames;  /* Queued */
    uint32_t bytes;   /* Queued */
    uint32_t refused; /* Did not fit; nothing of them was queued */
} uart_link_stats_t;

void uart_link_init(void);

/* Bytes that can be queued right now. */
uint32_t uart_link_free(void);

/* Queues len bytes as one unit and returns at once. False, with nothing queued, if they do not
 * all fit. */
bool uart_link_send(const uint8_t *data, uint32_t len);

void uart_link_get_stats(uart_link_stats_t *out);

#endif
