/* Slave application (nodes 2-4). */
#if NODE_ID < 2 || NODE_ID > 4
#error "app_slave.c is for nodes 2-4 only"
#endif

#include "drivers/board.h"
#include "drivers/systick.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

#define TICK_PERIOD_MS 1000U
#define TASK_COUNT (sizeof(tasks) / sizeof(tasks[0]))

static void tick_task(void)
{
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " tick t=");
    uart_debug_write_u32(systick_now_ms());
    uart_debug_write("\n");
}

static sched_task_t tasks[] = {
    { tick_task, TICK_PERIOD_MS, 0U },
};

int main(void)
{
    board_init();
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " boot\n");

    for (;;) {
        sched_run_due(tasks, TASK_COUNT);
    }
}
