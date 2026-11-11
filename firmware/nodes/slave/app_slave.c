/* Slave application (nodes 2-4). */
#if NODE_ID < 2 || NODE_ID > 4
#error "app_slave.c is for nodes 2-4 only"
#endif

#include "comm/comm.h"
#include "params/param_source.h"
#include "params/params.h"
#include "drivers/board.h"
#include "drivers/systick.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

/* Design Document §5.8 and §10. */
#define PARAM_PERIOD_MS     10U
#define HEARTBEAT_PERIOD_MS 100U
#define FRESHNESS_PERIOD_MS 5U

/* The report runs 5 ms after boot and then every second, so it always falls halfway between
 * two 10 ms sends and no frame is on the bus when the counters are printed. The scheduler
 * compares (now - last) >= period, so a start value of 5 - 1000 makes the first run at 5 ms. */
#define REPORT_PERIOD_MS 1000U
#define REPORT_FIRST_MS  5U

/* §7.3: the first heartbeat goes out at boot, before the first parameter block at 10 ms, so a
 * restart is announced before its data arrives. One node's frames leave in the order queued (Tx
 * FIFO mode), whatever their IDs. Start value 0 - 100 makes the first run at 0 ms. */
#define HEARTBEAT_FIRST_MS 0U

#define TASK_COUNT (sizeof(tasks) / sizeof(tasks[0]))

/* §6.3: samples the own parameters whose sample period has passed, then sends all five. Both
 * in one task with one time value, so a block always carries the newest samples. (As two
 * tasks, a SysTick between their due checks once made a block carry the previous samples.) */
static void param_task(void)
{
    comm_param_block_t block;

    param_source_task(systick_now_ms());
    params_build_block(&block);
    (void)comm_send_param_block(&block); /* A full Tx FIFO is counted inside comm and fdcan */
}

/* §6.2: freshness of all 20 parameters every 5 ms. */
static void freshness_task(void)
{
    params_update_freshness(systick_now_ms());
}

/* §5.3: saturating sum of this node's error counters, comm/ and params/ together. */
static uint16_t error_summary(void)
{
    uint32_t rejected = params_value_rejected();
    uint32_t sum = (uint32_t)comm_error_summary() + ((rejected > 0xFFFFU) ? 0xFFFFU : rejected);

    return (sum > 0xFFFFU) ? 0xFFFFU : (uint16_t)sum;
}

/* Stage 4 stand-in: the state comes from diag/ once it exists. Until then the node truthfully
 * reports BOOT. */
static void heartbeat_task(void)
{
    comm_heartbeat_t hb = {
        .state = COMM_STATE_BOOT,
        .synced_count = params_synced_count(),
        .uptime_ms = systick_now_ms(),
        .error_summary = error_summary(),
    };

    (void)comm_send_heartbeat(&hb);
}

static void report_task(void)
{
    comm_debug_report();
    params_debug_report();
}

static sched_task_t tasks[] = {
    { param_task, PARAM_PERIOD_MS, 0U },
    { freshness_task, FRESHNESS_PERIOD_MS, 0U },
    { heartbeat_task, HEARTBEAT_PERIOD_MS, HEARTBEAT_FIRST_MS - HEARTBEAT_PERIOD_MS },
    { report_task, REPORT_PERIOD_MS, REPORT_FIRST_MS - REPORT_PERIOD_MS },
};

int main(void)
{
    board_init();
    params_init();
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " boot\n");

    if (!comm_init()) {
        /* Fail fast: without CAN this node has nothing to do. The UART interrupt still sends
         * this line. */
        uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " CAN init FAILED\n");
        for (;;) {
        }
    }

    for (;;) {
        sched_run_due(tasks, TASK_COUNT);

        /* Drain the CAN RX ring on every pass (Design Document §4.1). comm validates and counts
         * each frame; params/ takes the accepted parameter blocks, and diag/ will take the
         * heartbeats in a later stage. */
        comm_frame_t frame;
        while (comm_receive(&frame)) {
            if (frame.type == COMM_MSG_PARAM) {
                params_apply_block(frame.source, &frame.body.param, frame.sequence, frame.rx_time_ms);
            }
        }
    }
}
