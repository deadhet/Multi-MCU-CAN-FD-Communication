/* FDCAN self test in internal loop back mode. Not a product image.
 *
 * Phase 1 sends one frame per case and checks what comes back: ID, length, FD and BRS flags,
 * every payload byte, and that the hardware filters block IDs outside the ranges.
 * Phase 2 stops reading, puts one frame into queue 1, then floods queue 0 with 40 frames. Queue
 * 0 must keep the newest 8 in order, and queue 1 must still hold its frame untouched. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "drivers/board.h"
#include "drivers/fdcan.h"
#include "drivers/frame_queue.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define STEP_PERIOD_MS 5U
#define OVERFLOW_FRAMES 40U
#define ACCEPTED_CASES 4U
#define QUEUE_FLOOD 0U    /* 0x031-0x034 */
#define QUEUE_QUIET 1U    /* 0x021-0x024 */
#define QUIET_SEED 100U   /* Payload seed of the single frame left in the quiet queue */
#define TASK_COUNT (sizeof(tasks) / sizeof(tasks[0]))

typedef struct {
    uint16_t id;
    uint8_t len;
    bool accepted;
} test_case_t;

static const fdcan_id_range_t ranges[] = {
    { 0x021U, 0x024U, QUEUE_QUIET },
    { 0x031U, 0x034U, QUEUE_FLOOD },
};

static const test_case_t cases[] = {
    { 0x021U, 32U, true },
    { 0x024U, 32U, true },
    { 0x031U, 16U, true },
    { 0x034U, 16U, true },
    { 0x011U, 16U, false }, /* Below the first range */
    { 0x025U, 32U, false }, /* Between the ranges */
    { 0x035U, 16U, false }, /* Above the last range */
};

#define CASE_COUNT (sizeof(cases) / sizeof(cases[0]))

typedef enum {
    PHASE_CASES,
    PHASE_SETTLE,
    PHASE_QUIET,
    PHASE_OVERFLOW,
    PHASE_DONE
} phase_t;

static phase_t phase = PHASE_CASES;
static uint32_t next_case;
static uint32_t expected_case;
static uint32_t received;
static uint32_t bad;
static uint32_t overflow_sent;
static bool stopped;

static uint8_t pattern_byte(uint32_t seed, uint32_t index)
{
    return (uint8_t)((seed * 31U) + (index * 7U) + 1U);
}

static void fill_payload(uint8_t *data, uint32_t seed, uint8_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        data[i] = pattern_byte(seed, i);
    }
}

static void print_number_field(const char *name, uint32_t value)
{
    uart_debug_write(name);
    uart_debug_write_u32(value);
}

/* The next case that should come back, skipping the ones the filters must block. */
static const test_case_t *next_expected(void)
{
    while ((expected_case < CASE_COUNT) && !cases[expected_case].accepted) {
        expected_case++;
    }
    return (expected_case < CASE_COUNT) ? &cases[expected_case] : NULL;
}

static void check_frame(const can_rx_slot_t *frame)
{
    const test_case_t *expected = next_expected();
    bool ok = (expected != NULL) && (frame->id == expected->id) && (frame->len == expected->len)
           && (frame->flags == (CAN_RX_FLAG_FD | CAN_RX_FLAG_BRS));

    if (ok) {
        for (uint32_t i = 0U; i < frame->len; i++) {
            ok = ok && (frame->data[i] == pattern_byte(expected_case, i));
        }
    }

    received++;
    expected_case++;
    bad += ok ? 0U : 1U;

    print_number_field("LOOPBACK rx id=", frame->id);
    print_number_field(" len=", frame->len);
    print_number_field(" fd=", frame->flags & CAN_RX_FLAG_FD);
    print_number_field(" brs=", (frame->flags & CAN_RX_FLAG_BRS) >> 1);
    print_number_field(" t=", frame->rx_time_ms);
    uart_debug_write(ok ? " ok\n" : " BAD\n");
}

static void finish_overflow(void)
{
    can_rx_slot_t frame;
    fdcan_stats_t stats;
    uint32_t stored = 0U;
    uint32_t quiet = 0U;
    uint32_t wrong = 0U;

    /* Flooded queue: the oldest 32 were given up, so frame k read here is frame 32 + k sent. */
    while (fdcan_receive(QUEUE_FLOOD, &frame)) {
        uint32_t seed = (OVERFLOW_FRAMES - FRAME_QUEUE_DEPTH) + stored;

        wrong += ((frame.id == 0x031U) && (frame.len == 16U)) ? 0U : 1U;
        for (uint32_t i = 0U; i < frame.len; i++) {
            wrong += (frame.data[i] == pattern_byte(seed, i)) ? 0U : 1U;
        }
        stored++;
    }
    /* Quiet queue: its one frame must have survived the flood next door. */
    while (fdcan_receive(QUEUE_QUIET, &frame)) {
        wrong += ((frame.id == 0x022U) && (frame.len == 32U)) ? 0U : 1U;
        for (uint32_t i = 0U; i < frame.len; i++) {
            wrong += (frame.data[i] == pattern_byte(QUIET_SEED, i)) ? 0U : 1U;
        }
        quiet++;
    }
    fdcan_get_stats(&stats);

    print_number_field("LOOPBACK queues sent=", overflow_sent);
    print_number_field(" stored=", stored);
    print_number_field(" overwritten=", stats.rx_overwritten[QUEUE_FLOOD]);
    print_number_field(" quiet=", quiet);
    print_number_field(" quiet_overwritten=", stats.rx_overwritten[QUEUE_QUIET]);
    print_number_field(" wrong=", wrong);
    uart_debug_write("\n");

    bool pass = (bad == 0U) && (received == ACCEPTED_CASES) && (stored == FRAME_QUEUE_DEPTH)
             && (stats.rx_overwritten[QUEUE_FLOOD] == (OVERFLOW_FRAMES - FRAME_QUEUE_DEPTH))
             && (quiet == 1U) && (stats.rx_overwritten[QUEUE_QUIET] == 0U) && (wrong == 0U)
             && (stats.rx_unrouted == 0U);
    uart_debug_write(pass ? "LOOPBACK PASS\n" : "LOOPBACK FAIL\n");
}

static void step_task(void)
{
    uint8_t data[CAN_MAX_DATA];

    switch (phase) {
    case PHASE_CASES:
        fill_payload(data, next_case, cases[next_case].len);
        if (fdcan_send(cases[next_case].id, data, cases[next_case].len)) {
            next_case++;
        }
        if (next_case == CASE_COUNT) {
            phase = PHASE_SETTLE;
        }
        break;

    case PHASE_SETTLE:
        print_number_field("LOOPBACK filter sent=", CASE_COUNT);
        print_number_field(" received=", received);
        print_number_field(" bad=", bad);
        uart_debug_write("\n");

        /* 13 is not a CAN FD length, so the driver must refuse it. */
        if (fdcan_send(0x021U, data, 13U)) {
            bad++;
        }
        print_number_field("LOOPBACK bad length refused=", (bad == 0U) ? 1U : 0U);
        uart_debug_write("\n");
        phase = PHASE_QUIET;
        break;

    case PHASE_QUIET:
        fill_payload(data, QUIET_SEED, 32U);
        if (fdcan_send(0x022U, data, 32U)) {
            phase = PHASE_OVERFLOW;
        }
        break;

    case PHASE_OVERFLOW:
        fill_payload(data, overflow_sent, 16U);
        if (fdcan_send(0x031U, data, 16U)) {
            overflow_sent++;
        }
        if (overflow_sent == OVERFLOW_FRAMES) {
            phase = PHASE_DONE;
        }
        break;

    case PHASE_DONE:
        finish_overflow();
        stopped = true;
        break;

    default:
        break;
    }
}

static sched_task_t tasks[] = {
    { step_task, STEP_PERIOD_MS, 0U, "step" },
};
_Static_assert(TASK_COUNT <= SCHED_MAX_TASKS, "more tasks than the scheduler measures");

int main(void)
{
    can_rx_slot_t frame;

    board_init();
    sched_init(tasks, TASK_COUNT);
    uart_debug_write("LOOPBACK start\n");

    if (!fdcan_init(FDCAN_MODE_LOOPBACK, ranges, sizeof(ranges) / sizeof(ranges[0]))) {
        uart_debug_write("LOOPBACK FAIL init\n");
        stopped = true;
    }

    for (;;) {
        if (!stopped) {
            sched_run_due();
        }

        /* Phase 2 must leave frames in the queues on purpose. In phase 1 only one frame is in
         * flight at a time, so reading both queues keeps the send order. */
        if ((phase <= PHASE_SETTLE)
            && (fdcan_receive(QUEUE_FLOOD, &frame) || fdcan_receive(QUEUE_QUIET, &frame))) {
            check_frame(&frame);
        }
    }
}
