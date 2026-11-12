#ifndef BOARD_H
#define BOARD_H

/* All pin and clock setup lives behind this call. */
void board_init(void);

/* USART3 TX pin and clock for the UART link. MCU1 only; called by uart_link_init(). */
void board_uart_link_pins_init(void);

#endif
