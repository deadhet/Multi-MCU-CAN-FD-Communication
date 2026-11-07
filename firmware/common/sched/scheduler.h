#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

typedef struct {
    void (*run)(void);
    uint32_t period_ms;
    uint32_t last_run_ms;
} sched_task_t;

/* Runs every task whose period has passed. Call on every pass of the main loop.
 * Tasks are cooperative, so each one must be short and must never wait. */
void sched_run_due(sched_task_t *tasks, uint32_t count);

#endif
