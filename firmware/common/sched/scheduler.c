#include <stddef.h>
#include "drivers/cycles.h"
#include "drivers/systick.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

static sched_task_t *table;
static uint32_t table_count;
static sched_stats_t stats[SCHED_MAX_TASKS]; /* Index as in the table */
static uint32_t pass_start;
static bool pass_started;
static uint32_t pass_max;

void sched_init(sched_task_t *tasks, uint32_t count)
{
    table = tasks;
    table_count = (count <= SCHED_MAX_TASKS) ? count : SCHED_MAX_TASKS;
    for (uint32_t i = 0U; i < SCHED_MAX_TASKS; i++) {
        stats[i] = (sched_stats_t){ 0 };
    }
    pass_started = false;
    pass_max = 0U;
    cycles_init();
}

bool sched_get_stats(uint32_t index, sched_stats_t *out)
{
    if (index >= table_count) {
        return false;
    }
    *out = stats[index];
    return true;
}

void sched_run_due(void)
{
    for (uint32_t i = 0U; i < table_count; i++) {
        sched_task_t *t = &table[i];
        uint32_t now = systick_now_ms();
        uint32_t since = now - t->last_run_ms; /* Unsigned: correct when the ms counter wraps */

        if (since >= t->period_ms) {
            uint32_t late = since - t->period_ms;
            t->last_run_ms = now;

            uint32_t start = cycles_now();
            t->run();
            uint32_t took = cycles_now() - start; /* Unsigned: correct across a counter wrap */

            sched_stats_t *s = &stats[i];
            s->runs++;
            if (took > s->run_max_cycles) {
                s->run_max_cycles = took;
            }
            if (late > 0U) {
                s->late_runs++;
                if (late > s->late_max_ms) {
                    s->late_max_ms = late;
                }
            }
        }
    }
}

void sched_pass_mark(void)
{
    uint32_t now = cycles_now();

    if (pass_started && ((now - pass_start) > pass_max)) {
        pass_max = now - pass_start;
    }
    pass_start = now;
    pass_started = true;
}

uint32_t sched_pass_max_cycles(void)
{
    return pass_max;
}

/* Rounded up, so a run that took any time at all never shows as 0 us. */
static uint32_t to_us(uint32_t cycles)
{
    return (cycles / CYCLES_PER_US) + (((cycles % CYCLES_PER_US) != 0U) ? 1U : 0U);
}

void sched_debug_report(void)
{
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " sched pass=");
    uart_debug_write_u32(to_us(pass_max));
    uart_debug_write("us");
    for (uint32_t i = 0U; i < table_count; i++) {
        uart_debug_write(" ");
        uart_debug_write((table[i].name != NULL) ? table[i].name : "task");
        uart_debug_write("=");
        uart_debug_write_u32(to_us(stats[i].run_max_cycles));
        uart_debug_write("us/");
        uart_debug_write_u32(stats[i].late_max_ms);
        uart_debug_write("ms/");
        uart_debug_write_u32(stats[i].late_runs);
    }
    uart_debug_write("\n");
}
