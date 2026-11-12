/* PC test for diag/diag.c, built as node 1 (the master's view of nodes 2-4). Times are passed
 * in, so every timeout can be driven at exact milliseconds.
 *   firmware/tests/host/run_host_tests.sh */
#include <stdio.h>
#include <string.h>
#include "diag/diag.h"

#if NODE_ID != 1
#error "test_diag.c expects NODE_ID=1"
#endif

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* Debug UART stand-in: collects everything diag/ prints. */
static char log_buf[8192];

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

static void log_clear(void)
{
    log_buf[0] = '\0';
}

static int logged(const char *text)
{
    return strstr(log_buf, text) != NULL;
}

static const bool no_data[COMM_NODE_COUNT] = { false, false, false, false };

static void hb(uint8_t node, comm_node_state_t state, uint32_t uptime, uint32_t t)
{
    comm_heartbeat_t h = { .state = (uint8_t)state, .synced_count = 20U, .uptime_ms = uptime };
    diag_on_heartbeat(node, &h, false, t);
}

static diag_link_t link_of(uint8_t node)
{
    diag_peer_t p;
    memset(&p, 0xA5, sizeof(p));
    CHECK(diag_get_peer(node, 0U, &p));
    return p.link;
}

static diag_peer_t peer(uint8_t node, uint32_t now)
{
    diag_peer_t p;
    memset(&p, 0xA5, sizeof(p));
    CHECK(diag_get_peer(node, now, &p));
    return p;
}

/* All three peers send a heartbeat and a block at t, so they stay ONLINE. */
static void all_alive(uint32_t t)
{
    for (uint8_t n = 2U; n <= 4U; n++) {
        hb(n, COMM_STATE_OPERATIONAL, t, t);
        diag_on_param_block(n, t);
    }
}

static void test_init_and_boot(void)
{
    printf("init, BOOT -> RESYNCING -> OPERATIONAL\n");
    diag_init();
    diag_peer_t p;
    CHECK(!diag_get_peer(1U, 0U, &p)); /* This node */
    CHECK(!diag_get_peer(0U, 0U, &p));
    CHECK(!diag_get_peer(5U, 0U, &p));
    CHECK(link_of(2U) == DIAG_UNKNOWN && link_of(3U) == DIAG_UNKNOWN && link_of(4U) == DIAG_UNKNOWN);
    CHECK(diag_own_state() == COMM_STATE_BOOT);

    /* Alone: stays BOOT whatever the time, until a heartbeat arrives. */
    CHECK(diag_update(5U, no_data) == 0U);
    CHECK(diag_own_state() == COMM_STATE_BOOT);

    log_clear();
    hb(2U, COMM_STATE_BOOT, 0U, 6U);
    CHECK(link_of(2U) == DIAG_ONLINE);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* One heartbeat from any node */
    CHECK(logged("MCU1 t=6 diag n2 ONLINE\n"));
    CHECK(logged("MCU1 t=6 diag self RESYNCING\n"));

    hb(3U, COMM_STATE_BOOT, 0U, 7U);
    diag_on_param_block(2U, 8U);
    diag_update(10U, no_data);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* Node 3 heard, no block from it yet */
    diag_on_param_block(3U, 12U);
    log_clear();
    diag_update(15U, no_data);
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL); /* Node 4 never heard: not waited for */
    CHECK(logged("MCU1 t=15 diag self OPERATIONAL\n"));

    /* Own heartbeat-less ID values are ignored. */
    hb(1U, COMM_STATE_BOOT, 0U, 16U);
    hb(5U, COMM_STATE_BOOT, 0U, 16U);
    diag_on_param_block(1U, 16U);
    diag_on_param_block(0U, 16U);
}

static void test_never_heard(void)
{
    printf("never heard: UNKNOWN, OFFLINE 350 ms after boot\n");
    diag_init();
    CHECK(diag_update(349U, no_data) == 0U);
    CHECK(link_of(4U) == DIAG_UNKNOWN);   /* No LATE for a node never heard */
    CHECK(peer(4U, 349U).missed == 2U);
    log_clear();
    CHECK(diag_update(350U, no_data) == 0x0EU); /* Nodes 2, 3, 4 went OFFLINE */
    CHECK(link_of(4U) == DIAG_OFFLINE && diag_peer_offline(4U));
    CHECK(peer(4U, 350U).missed == 3U && !peer(4U, 350U).heard);
    CHECK(logged("MCU1 t=350 diag n4 OFFLINE\n"));
    CHECK(diag_update(355U, no_data) == 0U); /* Reported once only */
    CHECK(diag_own_state() == COMM_STATE_BOOT);
}

static void test_late_offline(void)
{
    printf("LATE at 150 ms, missed count, OFFLINE at 350 ms\n");
    diag_init();
    all_alive(1000U);
    diag_update(1000U, no_data);
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL);

    all_alive(1100U); /* Nodes 2 and 4 keep going; node 3 stops after 1100 */
    hb(2U, COMM_STATE_OPERATIONAL, 1200U, 1200U);
    hb(4U, COMM_STATE_OPERATIONAL, 1200U, 1200U);
    diag_on_param_block(2U, 1200U);
    diag_on_param_block(4U, 1200U);
    CHECK(diag_update(1249U, no_data) == 0U);
    CHECK(link_of(3U) == DIAG_ONLINE && peer(3U, 1249U).missed == 0U);
    log_clear();
    diag_update(1250U, no_data);
    CHECK(link_of(3U) == DIAG_LATE && peer(3U, 1250U).missed == 1U);
    CHECK(logged("MCU1 t=1250 diag n3 LATE\n"));

    /* A late heartbeat brings it straight back: never OFFLINE, no recovery needed. */
    hb(3U, COMM_STATE_OPERATIONAL, 1300U, 1300U);
    CHECK(link_of(3U) == DIAG_ONLINE && peer(3U, 1300U).missed == 0U);

    hb(2U, COMM_STATE_OPERATIONAL, 1400U, 1400U);
    hb(4U, COMM_STATE_OPERATIONAL, 1400U, 1400U);
    diag_on_param_block(2U, 1400U);
    diag_on_param_block(4U, 1400U);
    hb(2U, COMM_STATE_OPERATIONAL, 1500U, 1500U);
    hb(4U, COMM_STATE_OPERATIONAL, 1500U, 1500U);
    diag_on_param_block(2U, 1500U);
    diag_on_param_block(4U, 1500U);
    diag_update(1549U, no_data);
    CHECK(peer(3U, 1549U).missed == 1U);
    diag_update(1550U, no_data);
    CHECK(peer(3U, 1550U).missed == 2U && link_of(3U) == DIAG_LATE);
    hb(2U, COMM_STATE_OPERATIONAL, 1600U, 1600U);
    hb(4U, COMM_STATE_OPERATIONAL, 1600U, 1600U);
    diag_on_param_block(2U, 1600U);
    diag_on_param_block(4U, 1600U);
    CHECK(diag_update(1649U, no_data) == 0U);
    log_clear();
    CHECK(diag_update(1650U, no_data) == 0x04U); /* Node 3 */
    CHECK(link_of(3U) == DIAG_OFFLINE && peer(3U, 1650U).missed == 3U);
    CHECK(logged("MCU1 t=1650 diag n3 OFFLINE\n"));
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL); /* Others coming and going: no change */
}

static void test_recovery_needs_second_heartbeat(void)
{
    printf("back after OFFLINE: first heartbeat RECOVERING only, a later OPERATIONAL one ONLINE\n");
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, 100U, 100U);
    diag_update(450U, no_data);
    CHECK(link_of(2U) == DIAG_OFFLINE);

    /* An old heartbeat from the sender's Tx FIFO that still says OPERATIONAL. */
    log_clear();
    hb(2U, COMM_STATE_OPERATIONAL, 200U, 1500U);
    CHECK(link_of(2U) == DIAG_RECOVERING);
    CHECK(!diag_peer_offline(2U));
    CHECK(logged("MCU1 t=1500 diag n2 RECOVERING\n"));
    CHECK(peer(2U, 1500U).recoveries == 0U && peer(2U, 1500U).restarts == 0U);

    hb(2U, COMM_STATE_RESYNCING, 1600U, 1600U);
    CHECK(link_of(2U) == DIAG_RECOVERING); /* Not OPERATIONAL yet */
    diag_update(1800U, no_data);
    CHECK(link_of(2U) == DIAG_RECOVERING); /* 200 ms silent: no LATE while recovering */
    log_clear();
    hb(2U, COMM_STATE_OPERATIONAL, 1850U, 1850U);
    CHECK(link_of(2U) == DIAG_ONLINE && peer(2U, 1850U).recoveries == 1U);
    CHECK(logged("MCU1 t=1850 diag n2 ONLINE recovered\n"));
}

static void test_restart(void)
{
    printf("restart: comm/'s flag, or uptime down while OFFLINE\n");
    diag_init();
    hb(3U, COMM_STATE_OPERATIONAL, 5000U, 5000U);
    log_clear();
    comm_heartbeat_t boot = { .state = (uint8_t)COMM_STATE_BOOT, .uptime_ms = 0U };
    diag_on_heartbeat(3U, &boot, true, 5020U);
    CHECK(peer(3U, 5020U).restarts == 1U && link_of(3U) == DIAG_RECOVERING);
    CHECK(logged("MCU1 t=5020 diag n3 RESTART\n"));
    hb(3U, COMM_STATE_OPERATIONAL, 100U, 5120U);
    CHECK(link_of(3U) == DIAG_ONLINE && peer(3U, 5120U).recoveries == 1U);
    CHECK(peer(3U, 5120U).restarts == 1U); /* Counted once */

    /* Scenario S2: OFFLINE, comm/ forgot it, so no flag; the uptime went down. */
    diag_update(5600U, no_data);
    CHECK(link_of(3U) == DIAG_OFFLINE);
    hb(3U, COMM_STATE_BOOT, 0U, 10000U);
    CHECK(peer(3U, 10000U).restarts == 2U && link_of(3U) == DIAG_RECOVERING);

    /* A halted node resumes with its uptime still rising: not a restart. */
    hb(3U, COMM_STATE_OPERATIONAL, 100U, 10100U);
    diag_update(10600U, no_data);
    hb(3U, COMM_STATE_OPERATIONAL, 200U, 12000U);
    CHECK(peer(3U, 12000U).restarts == 2U);
}

static void test_uptime_wrap(void)
{
    const uint32_t top = 0xFFFFFFFFU; /* A peer's last uptime before its count wraps to 0 */

    printf("peer's uptime passes 2^32 (49.7 days): a wrap, not a restart; 1000 ms slack, exactly\n");

    /* Steady heartbeats across the peer's wrap: stays ONLINE, nothing logged. */
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, top - 149U, 1000U);
    hb(2U, COMM_STATE_OPERATIONAL, top - 49U, 1100U);
    log_clear();
    hb(2U, COMM_STATE_OPERATIONAL, 50U, 1200U); /* 100 ms on, through 2^32 */
    hb(2U, COMM_STATE_OPERATIONAL, 150U, 1300U);
    CHECK(link_of(2U) == DIAG_ONLINE && peer(2U, 1300U).restarts == 0U);
    CHECK(!logged("RESTART") && !logged("RECOVERING"));

    /* Silent for 2 s across its wrap, back without a reset: RECOVERING, as after any outage,
     * but no restart. */
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, top - 999U, 1000U);
    diag_update(1400U, no_data);
    CHECK(link_of(2U) == DIAG_OFFLINE);
    hb(2U, COMM_STATE_OPERATIONAL, 1000U, 3000U); /* 2000 ms on, 2000 ms passed here */
    CHECK(link_of(2U) == DIAG_RECOVERING && peer(2U, 3000U).restarts == 0U);

    /* The slack: the step through 2^32 may exceed the time passed by 1000 ms (a wrap)... */
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, top - 99U, 1000U);
    hb(2U, COMM_STATE_OPERATIONAL, 1000U, 1100U); /* step 1100, passed 100 */
    CHECK(peer(2U, 1100U).restarts == 0U && link_of(2U) == DIAG_ONLINE);
    /* ...but not by 1001 ms (a restart). */
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, top - 99U, 1000U);
    hb(2U, COMM_STATE_OPERATIONAL, 1001U, 1100U); /* step 1101, passed 100 */
    CHECK(peer(2U, 1100U).restarts == 1U && link_of(2U) == DIAG_RECOVERING);
}

static void test_old_heartbeat_then_boot(void)
{
    printf("after OFFLINE: an old OPERATIONAL heartbeat, then a BOOT one (reset right after reconnect)\n");
    diag_init();
    hb(3U, COMM_STATE_OPERATIONAL, 1400U, 1400U);
    diag_update(1750U, no_data);
    CHECK(link_of(3U) == DIAG_OFFLINE);

    /* The old heartbeat from the sender's Tx FIFO, sent at 1500 and still saying OPERATIONAL. */
    hb(3U, COMM_STATE_OPERATIONAL, 1500U, 2500U);
    CHECK(link_of(3U) == DIAG_RECOVERING && peer(3U, 2500U).restarts == 0U);

    /* The node resets: its boot heartbeat says BOOT with uptime 0; comm/ flags the restart
     * because the sequence went back after the old heartbeat was accepted. */
    log_clear();
    comm_heartbeat_t boot = { .state = (uint8_t)COMM_STATE_BOOT, .uptime_ms = 0U };
    diag_on_heartbeat(3U, &boot, true, 2503U);
    CHECK(link_of(3U) == DIAG_RECOVERING);
    CHECK(peer(3U, 2503U).restarts == 1U && peer(3U, 2503U).recoveries == 0U);
    CHECK(logged("MCU1 t=2503 diag n3 RESTART\n"));
    CHECK(!logged("RECOVERING")); /* Already RECOVERING: no second line */

    hb(3U, COMM_STATE_OPERATIONAL, 100U, 2603U); /* Its next heartbeat, after it resynchronised */
    CHECK(link_of(3U) == DIAG_ONLINE);
    CHECK(peer(3U, 2603U).restarts == 1U && peer(3U, 2603U).recoveries == 1U);
}

static void test_no_heartbeat(void)
{
    printf("heartbeats stop but data still proper: NO_HEARTBEAT, not OFFLINE\n");
    diag_init();
    bool data[COMM_NODE_COUNT] = { false, true, false, false }; /* Node 2's parameters synced */
    hb(2U, COMM_STATE_OPERATIONAL, 100U, 100U);
    diag_update(250U, data);
    CHECK(link_of(2U) == DIAG_LATE);
    log_clear();
    CHECK((diag_update(450U, data) & 0x02U) == 0U); /* Not reported as OFFLINE */
    CHECK(link_of(2U) == DIAG_NO_HEARTBEAT && !diag_peer_offline(2U));
    CHECK(logged("MCU1 t=450 diag n2 NO_HEARTBEAT\n"));
    CHECK(peer(2U, 650U).missed == 3U); /* Last computed at 450 */

    /* Its heartbeat returns: it was never OFFLINE, so straight back ONLINE. */
    hb(2U, COMM_STATE_OPERATIONAL, 700U, 700U);
    CHECK(link_of(2U) == DIAG_ONLINE);

    /* Data stops too: OFFLINE. Data back without heartbeats: NO_HEARTBEAT again. Then the
     * heartbeat, after an OFFLINE, goes through RECOVERING. */
    diag_update(1050U, data);
    CHECK(link_of(2U) == DIAG_NO_HEARTBEAT);
    data[1] = false;
    CHECK(diag_update(1060U, data) == 0x02U);
    CHECK(link_of(2U) == DIAG_OFFLINE);
    data[1] = true;
    CHECK(diag_update(1100U, data) == 0U);
    CHECK(link_of(2U) == DIAG_NO_HEARTBEAT);
    hb(2U, COMM_STATE_OPERATIONAL, 1150U, 1150U);
    CHECK(link_of(2U) == DIAG_RECOVERING);
    hb(2U, COMM_STATE_OPERATIONAL, 1250U, 1250U);
    CHECK(link_of(2U) == DIAG_ONLINE);

    /* Heartbeats broken from boot, data fine: NO_HEARTBEAT at 350 ms, never OFFLINE. */
    diag_init();
    bool d3[COMM_NODE_COUNT] = { false, false, true, false };
    CHECK((diag_update(350U, d3) & 0x04U) == 0U);
    CHECK(link_of(3U) == DIAG_NO_HEARTBEAT && !peer(3U, 350U).heard);
}

static void test_own_state_alone(void)
{
    printf("own state: all peers OFFLINE -> RESYNCING, back -> OPERATIONAL\n");
    diag_init();
    all_alive(0U);
    diag_update(5U, no_data);
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL);

    /* Scenario S3 from the cut-off node's side: all three go OFFLINE. */
    log_clear();
    CHECK(diag_update(350U, no_data) == 0x0EU);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING);
    CHECK(logged("MCU1 t=350 diag self RESYNCING\n"));
    diag_update(5000U, no_data);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* Alone: never OPERATIONAL */

    /* Back: heartbeats first, then blocks. */
    hb(2U, COMM_STATE_OPERATIONAL, 5000U, 5000U);
    hb(3U, COMM_STATE_OPERATIONAL, 5000U, 5000U);
    diag_update(5005U, no_data);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* No block yet since they went OFFLINE */
    diag_on_param_block(2U, 5007U);
    diag_update(5010U, no_data);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* Node 3 still owes one */
    diag_on_param_block(3U, 5012U);
    diag_update(5015U, no_data);
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL);
}

static void test_wrap_and_report(void)
{
    printf("ms counter wrap, report line\n");
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, 1U, 0xFFFFFFF0U);
    diag_update(0x00000085U, no_data); /* 149 ms later, across the wrap */
    CHECK(link_of(2U) == DIAG_ONLINE);
    diag_update(0x00000086U, no_data); /* 150 */
    CHECK(link_of(2U) == DIAG_LATE);

    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, 900U, 900U);
    diag_on_param_block(2U, 900U);
    log_clear();
    diag_update(1000U, no_data);
    diag_debug_report(1005U);
    CHECK(logged("MCU1 diag self=OPERATIONAL n2=ONLINE/OPERATIONAL age=105 miss=0 r=0 rec=0 "
                 "n3=OFFLINE/- age=1005 miss=9 r=0 rec=0 n4=OFFLINE/- age=1005 miss=9 r=0 rec=0\n"));
}

/* Nodes 3 and 4 send heartbeats and blocks at t; node 2 only a heartbeat (its blocks are all
 * rejected, so diag/ never hears of them). */
static void round_without_n2_blocks(uint32_t t)
{
    for (uint8_t n = 2U; n <= 4U; n++) {
        hb(n, COMM_STATE_OPERATIONAL, t, t);
    }
    diag_on_param_block(3U, t);
    diag_on_param_block(4U, t);
}

static void test_data_invalid(void)
{
    printf("heartbeats fine, no accepted block for 350 ms: DATA_INVALID, not waited for\n");
    diag_init();
    for (uint32_t t = 0U; t <= 300U; t += 100U) {
        round_without_n2_blocks(t);
        diag_update(t + 5U, no_data);
        CHECK(diag_own_state() == COMM_STATE_RESYNCING); /* Waiting for node 2's data */
    }
    CHECK(diag_update(349U, no_data) == 0U);
    CHECK(link_of(2U) == DIAG_ONLINE);
    log_clear();
    CHECK(diag_update(350U, no_data) == 0U); /* Not OFFLINE */
    CHECK(link_of(2U) == DIAG_DATA_INVALID && !diag_peer_offline(2U));
    CHECK(peer(2U, 350U).data_age_ms == 350U && peer(2U, 350U).missed == 0U);
    CHECK(diag_own_state() == COMM_STATE_OPERATIONAL); /* No longer waited for */
    CHECK(logged("MCU1 t=350 diag n2 DATA_INVALID\n"));
    CHECK(logged("MCU1 t=350 diag self OPERATIONAL\n"));

    /* Its heartbeats keep coming: still DATA_INVALID, at once and after the next update (no
     * ONLINE/DATA_INVALID flapping on every heartbeat). An accepted block ends it. */
    log_clear();
    round_without_n2_blocks(400U);
    CHECK(link_of(2U) == DIAG_DATA_INVALID);
    diag_update(405U, no_data);
    CHECK(link_of(2U) == DIAG_DATA_INVALID);
    CHECK(!logged("diag n2"));
    log_clear();
    diag_on_param_block(2U, 420U);
    CHECK(link_of(2U) == DIAG_ONLINE);
    CHECK(logged("MCU1 t=420 diag n2 ONLINE\n"));

    /* Blocks stop again, heartbeats continue: DATA_INVALID at 420 + 350. */
    round_without_n2_blocks(500U);
    round_without_n2_blocks(600U);
    round_without_n2_blocks(700U);
    diag_update(769U, no_data);
    CHECK(link_of(2U) == DIAG_ONLINE);
    diag_update(770U, no_data);
    CHECK(link_of(2U) == DIAG_DATA_INVALID);

    /* Then its heartbeats stop too (last at 700): LATE, then OFFLINE (dead, not just data). */
    hb(3U, COMM_STATE_OPERATIONAL, 800U, 800U);
    hb(4U, COMM_STATE_OPERATIONAL, 800U, 800U);
    diag_update(850U, no_data);
    CHECK(link_of(2U) == DIAG_LATE);
    hb(3U, COMM_STATE_OPERATIONAL, 900U, 900U);
    hb(4U, COMM_STATE_OPERATIONAL, 900U, 900U);
    hb(3U, COMM_STATE_OPERATIONAL, 1000U, 1000U);
    hb(4U, COMM_STATE_OPERATIONAL, 1000U, 1000U);
    CHECK(diag_update(1050U, no_data) == 0x02U);
    CHECK(link_of(2U) == DIAG_OFFLINE);

    /* A node whose only peer sends no proper data is not OPERATIONAL. */
    diag_init();
    hb(2U, COMM_STATE_OPERATIONAL, 0U, 0U);
    hb(2U, COMM_STATE_OPERATIONAL, 100U, 100U);
    hb(2U, COMM_STATE_OPERATIONAL, 200U, 200U);
    hb(2U, COMM_STATE_OPERATIONAL, 300U, 300U);
    diag_update(350U, no_data);
    CHECK(link_of(2U) == DIAG_DATA_INVALID);
    CHECK(diag_own_state() == COMM_STATE_RESYNCING);
}

int main(void)
{
    test_init_and_boot();
    test_never_heard();
    test_late_offline();
    test_recovery_needs_second_heartbeat();
    test_restart();
    test_uptime_wrap();
    test_old_heartbeat_then_boot();
    test_no_heartbeat();
    test_data_invalid();
    test_own_state_alone();
    test_wrap_and_report();
    printf("%s (%d failures)\n", (failures == 0) ? "DIAG PASS" : "DIAG FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
