#include "drivers/systick.h"
#include "sched/scheduler.h"

void sched_run_due(sched_task_t *tasks, uint32_t count)
{
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t now = systick_now_ms();

        /* Unsigned subtraction stays correct when the ms counter wraps. */
        if ((now - tasks[i].last_run_ms) >= tasks[i].period_ms) {
            tasks[i].last_run_ms = now;
            tasks[i].run();
        }
    }
}
