/* Master application (node 1). */
#if NODE_ID != 1
#error "app_master.c is for node 1 only"
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
