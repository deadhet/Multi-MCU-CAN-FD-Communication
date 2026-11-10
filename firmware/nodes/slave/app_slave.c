/* Slave application (nodes 2-4). */
#if NODE_ID < 2 || NODE_ID > 4
#error "app_slave.c is for nodes 2-4 only"
#endif

#include "comm/comm.h"
#include "drivers/board.h"
#include "drivers/systick.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

/* Design Document §5.8 and §10. */
#define PARAM_PERIOD_MS     10U
#define HEARTBEAT_PERIOD_MS 100U

/* The report runs 5 ms after boot and then every second, so it always falls halfway between
 * two 10 ms sends and no frame is on the bus when the counters are printed. The scheduler
 * compares (now - last) >= period, so a start value of 5 - 1000 makes the first run at 5 ms. */
#define REPORT_PERIOD_MS 1000U
#define REPORT_FIRST_MS  5U

#define TASK_COUNT (sizeof(tasks) / sizeof(tasks[0]))

/* Stage 4 stand-in: values and status come from params/ once it exists. */
static void param_task(void)
{
    comm_param_block_t block = { 0 };

    (void)comm_send_param_block(&block); /* A full Tx FIFO is counted inside comm and fdcan */
}

/* Stage 4 stand-in: state and synced count come from diag/ once it exists. Until then the
 * node truthfully reports BOOT with nothing synced. */
static void heartbeat_task(void)
{
    comm_heartbeat_t hb = {
        .state = COMM_STATE_BOOT,
        .synced_count = 0U,
        .uptime_ms = systick_now_ms(),
        .error_summary = comm_error_summary(),
    };

    (void)comm_send_heartbeat(&hb);
}

static void report_task(void)
{
    comm_debug_report();
}

static sched_task_t tasks[] = {
    { param_task, PARAM_PERIOD_MS, 0U },
    { heartbeat_task, HEARTBEAT_PERIOD_MS, 0U },
    { report_task, REPORT_PERIOD_MS, REPORT_FIRST_MS - REPORT_PERIOD_MS },
};

int main(void)
{
    board_init();
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
         * each frame; params/ and diag/ will take the accepted ones in later stages. */
        comm_frame_t frame;
        while (comm_receive(&frame)) {
        }
    }
}
