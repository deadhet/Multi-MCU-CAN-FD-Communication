#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_debug_init(void);

/* Queues text for USART1 and returns at once. A message that does not fit is dropped whole. */
void uart_debug_write(const char *text);

uint32_t uart_debug_dropped(void);

#endif
