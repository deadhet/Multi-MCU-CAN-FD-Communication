/* Master application (node 1). */
#if NODE_ID != 1
#error "app_master.c is for node 1 only"
#endif

#include "comm/comm.h"
#include "diag/diag.h"
#include "monitor/monitor.h"
#include "params/param_source.h"
#include "params/params.h"
#include "drivers/board.h"
#include "drivers/systick.h"
#include "drivers/uart_link.h"
#include "drivers/uart.h"
#include "sched/scheduler.h"

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)

/* Design Document §5.8 and §10. */
#define PARAM_PERIOD_MS     10U
#define HEARTBEAT_PERIOD_MS 100U
#define FRESHNESS_PERIOD_MS 5U
#define DIAG_PERIOD_MS      5U

/* The report runs 5 ms after boot and then every second, so it always falls halfway between
 * two 10 ms sends and no frame is on the bus when the counters are printed. The scheduler
 * compares (now - last) >= period, so a start value of 5 - 1000 makes the first run at 5 ms. */
#define REPORT_PERIOD_MS 1000U
#define REPORT_FIRST_MS  5U

/* §7.3: the first heartbeat goes out at boot, before the first parameter block at 10 ms, so a
 * restart is announced before its data arrives. One node's frames leave in the order queued (Tx
 * FIFO mode), whatever their IDs. Start value 0 - 100 makes the first run at 0 ms. */
#define HEARTBEAT_FIRST_MS 0U

/* §8.3: a SNAPSHOT every 100 ms, a CONFIG at start-up and every second. Snapshots run 5 ms after
 * a 10 ms send, like the report, and right after it in the task table: the snapshot that follows
 * the debug report in the same pass shows the same values, so the two can be compared exactly.
 * (Before the report, its work moved the report to the next ms in Renode.) */
#define SNAPSHOT_PERIOD_MS 100U
#define SNAPSHOT_FIRST_MS  5U
#define CONFIG_PERIOD_MS   1000U
#define CONFIG_FIRST_MS    0U

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

/* §7.1: own state from diag/ (BOOT, RESYNCING, OPERATIONAL), synced count, uptime, errors. */
static void heartbeat_task(void)
{
    comm_heartbeat_t hb = {
        .state = (uint8_t)diag_own_state(),
        .synced_count = params_synced_count(),
        .uptime_ms = systick_now_ms(),
        .error_summary = error_summary(),
    };

    (void)comm_send_heartbeat(&hb);
}

/* §7.2: heartbeat timeouts and the own state, every 5 ms. diag/ calls no other service (§4.4),
 * so the app carries its results: a peer that went OFFLINE is forgotten in comm/, so its next
 * frame is accepted whatever its sequence (§5.6), and its parameters get the owner-offline flag
 * in params/ (Het, 11 Nov 2026). params/ tells diag/ whose data still arrives properly, which
 * makes a peer with silent heartbeats NO_HEARTBEAT instead of OFFLINE. */
static void diag_task(void)
{
    uint32_t now = systick_now_ms();
    bool data_ok[COMM_NODE_COUNT];

    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        data_ok[node - 1U] = params_node_synced(node);
    }
    uint8_t went_offline = diag_update(now, data_ok);
    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        if ((went_offline & (1U << (node - 1U))) != 0U) {
            comm_forget_peer(node);
        }
        params_set_owner_offline(node, diag_peer_offline(node), now);
    }
}

/* §8: the UART link. The app collects copies of the database and of every node's health, so
 * monitor/ calls no other service (§4.4). */
static void snapshot_task(void)
{
    static monitor_view_t view; /* Over 1 KB: static, not on the stack */

    view.now_ms = systick_now_ms();
    view.own_state = diag_own_state();
    view.synced_count = params_synced_count();
    view.own_error_summary = error_summary();
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        (void)params_get(id, view.now_ms, &view.params[id - 1U]);
    }
    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        (void)diag_get_peer(node, view.now_ms, &view.peers[node - 1U]); /* False for this node */
    }
    (void)monitor_send_snapshot(&view); /* A skipped snapshot is counted inside monitor/ */
}

static void config_task(void)
{
    static const monitor_periods_t periods = {
        .param_period_ms = PARAM_PERIOD_MS,
        .heartbeat_period_ms = HEARTBEAT_PERIOD_MS,
        .snapshot_period_ms = SNAPSHOT_PERIOD_MS,
    };

    (void)monitor_send_config(&periods);
}

static void report_task(void)
{
    comm_debug_report();
    params_debug_report();
    diag_debug_report(systick_now_ms());

    /* §8.4: debug text that does not fit in the 2 KB USART1 buffer is dropped whole and
     * counted; this line makes such losses visible. */
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " log dropped=");
    uart_debug_write_u32(uart_debug_dropped());
    uart_debug_write("\n");

    /* UART link: frames and bytes queued, frames that did not fit, snapshots and CONFIGs sent,
     * snapshots skipped (§8.4), last and largest snapshot on the wire in bytes. */
    uart_link_stats_t link;
    monitor_stats_t mon;
    uart_link_get_stats(&link);
    monitor_get_stats(&mon);
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " link frames=");
    uart_debug_write_u32(link.frames);
    uart_debug_write(" bytes=");
    uart_debug_write_u32(link.bytes);
    uart_debug_write(" refused=");
    uart_debug_write_u32(link.refused);
    uart_debug_write(" snap=");
    uart_debug_write_u32(mon.snapshots);
    uart_debug_write(" cfg=");
    uart_debug_write_u32(mon.configs);
    uart_debug_write(" skipped=");
    uart_debug_write_u32(mon.skipped);
    uart_debug_write(" enc_err=");
    uart_debug_write_u32(mon.encode_errors);
    uart_debug_write(" snap_bytes=");
    uart_debug_write_u32(mon.last_snapshot_bytes);
    uart_debug_write("/");
    uart_debug_write_u32(mon.max_snapshot_bytes);
    uart_debug_write("\n");
}

static sched_task_t tasks[] = {
    { param_task, PARAM_PERIOD_MS, 0U },
    { freshness_task, FRESHNESS_PERIOD_MS, 0U },
    { diag_task, DIAG_PERIOD_MS, 0U },
    { heartbeat_task, HEARTBEAT_PERIOD_MS, HEARTBEAT_FIRST_MS - HEARTBEAT_PERIOD_MS },
    { report_task, REPORT_PERIOD_MS, REPORT_FIRST_MS - REPORT_PERIOD_MS },
    { snapshot_task, SNAPSHOT_PERIOD_MS, SNAPSHOT_FIRST_MS - SNAPSHOT_PERIOD_MS },
    { config_task, CONFIG_PERIOD_MS, CONFIG_FIRST_MS - CONFIG_PERIOD_MS },
};

int main(void)
{
    board_init();
    params_init();
    diag_init();
    uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " boot\n");

    if (!comm_init()) {
        /* Fail fast: without CAN this node has nothing to do. The UART interrupt still sends
         * this line. */
        uart_debug_write("MCU" NUMBER_TEXT(NODE_ID) " CAN init FAILED\n");
        for (;;) {
        }
    }
    uart_link_init();
    monitor_init();

    for (;;) {
        sched_run_due(tasks, TASK_COUNT);

        /* Drain the CAN RX ring on every pass (Design Document §4.1). comm validates and counts
         * each frame; params/ takes the accepted parameter blocks and diag/ the heartbeats.
         * diag/ also hears of each accepted block, for its own-state rule.
         * Rejected frames reach neither. */
        comm_frame_t frame;
        while (comm_receive(&frame)) {
            if (frame.type == COMM_MSG_PARAM) {
                params_apply_block(frame.source, &frame.body.param, frame.sequence, frame.rx_time_ms);
                diag_on_param_block(frame.source, frame.rx_time_ms);
            } else {
                diag_on_heartbeat(frame.source, &frame.body.heartbeat, frame.restart, frame.rx_time_ms);
            }
        }
    }
}
