/* Slave application (nodes 2-4). */
#if NODE_ID < 2 || NODE_ID > 4
#error "app_slave.c is for nodes 2-4 only"
#endif

#include "drivers/board.h"
#include "drivers/uart.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

int main(void)
{
    board_init();
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " boot\n");

    for (;;) {
    }
}
