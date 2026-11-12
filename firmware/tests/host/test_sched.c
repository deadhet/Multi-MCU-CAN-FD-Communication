/* PC test for sched/scheduler.c: when tasks run (unchanged by the measurements) and what the
 * measurements record. The ms clock, the cycle counter and the debug UART are stand-ins.
 *   firmware/tests/host/run_host_tests.sh */
#include <stdio.h>
#include <string.h>
#include "drivers/cycles.h"
#include "drivers/systick.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* ---- Stand-ins ---- */
static uint32_t fake_ms;
static uint32_t fake_cycles;
static char log_buf[1024];

uint32_t systick_now_ms(void) { return fake_ms; }
void cycles_init(void) {}
uint32_t cycles_now(void) { return fake_cycles; }

void uart_debug_write(const char *text)
{
    strncat(log_buf, text, sizeof(log_buf) - strlen(log_buf) - 1U);
}

void uart_debug_write_u32(uint32_t value)
{
    char s[12];
    snprintf(s, sizeof(s), "%u", (unsigned)value);
    uart_debug_write(s);
}

/* ---- Tasks: each counts its runs and "takes" a set number of cycles ---- */
static uint32_t a_runs, b_runs, a_cost, b_cost;
static void task_a(void) { a_runs++; fake_cycles += a_cost; }
static void task_b(void) { b_runs++; fake_cycles += b_cost; }

static sched_task_t tasks[2];

static void setup(uint32_t a_first, uint32_t b_first)
{
    tasks[0] = (sched_task_t){ task_a, 10U, a_first - 10U, "a" };
    tasks[1] = (sched_task_t){ task_b, 100U, b_first - 100U, "b" };
    a_runs = b_runs = 0U;
    a_cost = b_cost = 0U;
    fake_ms = 0U;
    fake_cycles = 0U;
    sched_init(tasks, 2U);
}

static sched_stats_t stats_of(uint32_t i)
{
    sched_stats_t s;
    memset(&s, 0xA5, sizeof(s));
    CHECK(sched_get_stats(i, &s));
    return s;
}

static void at(uint32_t ms)
{
    fake_ms = ms;
    sched_run_due();
}

static void test_timing_unchanged(void)
{
    printf("When tasks run: unchanged (due when now - last >= period; a late run moves the next)\n");
    setup(0U, 5U);
    at(0U);
    CHECK(a_runs == 1U && b_runs == 0U);
    at(5U);
    CHECK(a_runs == 1U && b_runs == 1U);
    at(9U);
    CHECK(a_runs == 1U);
    at(10U);
    CHECK(a_runs == 2U);
    at(23U); /* 3 ms late */
    CHECK(a_runs == 3U);
    at(30U); /* Not yet: the next run is 10 ms after the late one (no fixed grid) */
    CHECK(a_runs == 3U);
    at(33U);
    CHECK(a_runs == 4U);
    at(90U); /* Long stall: one run, the missed ones are not caught up */
    CHECK(a_runs == 5U);
}

static void test_measurements(void)
{
    printf("Measurements: longest run, latest start, late runs, longest pass, report line\n");
    setup(0U, 5U);
    a_cost = 4800U; /* 10 us at 480 MHz */
    b_cost = 1U;    /* Under 1 us: shown as 1 us, never 0 */
    at(0U);
    at(5U);
    at(10U);
    sched_stats_t a = stats_of(0U);
    CHECK(a.runs == 2U && a.run_max_cycles == 4800U && a.late_max_ms == 0U && a.late_runs == 0U);

    a_cost = 960U; /* Shorter: the maximum stays */
    at(23U);       /* 3 ms late */
    at(34U);       /* 1 ms late */
    at(44U);       /* on time */
    a = stats_of(0U);
    CHECK(a.runs == 5U && a.run_max_cycles == 4800U && a.late_max_ms == 3U && a.late_runs == 2U);

    /* The cycle counter wraps every 8.9 s: a run across the wrap is still measured right. */
    fake_cycles = 0xFFFFFF00U;
    a_cost = 0x200U * 30U;
    at(54U);
    CHECK(stats_of(0U).run_max_cycles == 0x200U * 30U);

    /* Longest pass, from one mark to the next. */
    fake_cycles = 1000U;
    sched_pass_mark();
    fake_cycles = 3000U;
    sched_pass_mark(); /* 2000 */
    fake_cycles = 3500U;
    sched_pass_mark(); /* 500 */
    CHECK(sched_pass_max_cycles() == 2000U);

    log_buf[0] = '\0';
    sched_debug_report();
    CHECK(strcmp(log_buf, "MCU2 sched pass=5us a=32us/3ms/2 b=1us/0ms/0\n") == 0);
    if (failures != 0) {
        printf("  report: %s", log_buf);
    }

    sched_stats_t s;
    CHECK(!sched_get_stats(2U, &s));
}

static void test_wraps(void)
{
    printf("ms counter wrap at 2^32: still due on time, lateness right\n");
    tasks[0] = (sched_task_t){ task_a, 10U, 0xFFFFFFF8U, "a" };
    a_runs = 0U;
    a_cost = 0U;
    sched_init(tasks, 1U);
    at(0xFFFFFFFFU); /* 7 ms after the last run */
    CHECK(a_runs == 0U);
    at(2U); /* 10 ms after, across the wrap */
    CHECK(a_runs == 1U && stats_of(0U).late_max_ms == 0U);
    at(14U); /* 12 ms after: 2 ms late */
    CHECK(a_runs == 2U && stats_of(0U).late_max_ms == 2U);
}

int main(void)
{
    test_timing_unchanged();
    test_measurements();
    test_wraps();
    printf("%s (%d failures)\n", (failures == 0) ? "SCHED PASS" : "SCHED FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
