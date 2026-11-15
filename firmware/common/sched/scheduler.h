/* Cooperative, time-triggered task table. Every main-loop pass, sched_run_due() walks the table
 * in order and runs each task whose period has passed since its last run; then the app drains
 * the CAN queues. Tasks never interrupt each other, so each one must be short and must never
 * wait.
 *
 * Measurements (measure first, change the timing only if they show
 * lateness). Per task, worst cases since boot: its longest run, its latest start after it was
 * due, and how many runs started late; and the longest main-loop pass. They change nothing
 * about when tasks run. */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t runs;
    uint32_t run_max_cycles; /* Longest run, interrupts during it included */
    uint32_t late_max_ms;    /* Latest start, in ms after it was due */
    uint32_t late_runs;      /* Runs that started 1 ms or more after they were due */
} sched_stats_t;

typedef struct {
    void (*run)(void);
    uint32_t period_ms;
    uint32_t last_run_ms; /* First due time minus the period, to start later than at 0 ms */
    const char *name;     /* For the report */
} sched_task_t;

#define SCHED_MAX_TASKS 8U

/* Takes the task table and starts the cycle counter. Before the first sched_run_due(). At most
 * SCHED_MAX_TASKS tasks: each app checks this at compile time, after its table. */
void sched_init(sched_task_t *tasks, uint32_t count);

/* Copy of task `index`'s measurements. False if there is no such task. */
bool sched_get_stats(uint32_t index, sched_stats_t *out);

/* Runs every task whose period has passed. Call on every pass of the main loop. */
void sched_run_due(void);

/* Call at the top of every main-loop pass: measures the longest pass (the tasks that ran, the
 * CAN drain, and the interrupts in between). */
void sched_pass_mark(void);

uint32_t sched_pass_max_cycles(void);

/* One line, worst cases since boot:
 *   MCUn sched pass=<longest pass>us <name>=<longest run>us/<latest start>ms/<late runs> ... */
void sched_debug_report(void);

#endif
