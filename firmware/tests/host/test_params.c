/* PC test for system_table.c, params/params.c and params/param_source.c, built as node 2.
 * Times are passed in, so every rule can be driven at exact milliseconds.
 *   firmware/tests/host/run_host_tests.sh */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "params/param_source.h"
#include "params/params.h"
#include "system_table.h"

#if NODE_ID != 2
#error "test_params.c expects NODE_ID=2"
#endif

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* Debug UART stand-in: collects everything params/ prints. */
static char log_buf[4096];

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

/* Change count (events option b, Het 13 Nov 2026). After every params/ call that can change
 * something in every test below, params_change_count() must have moved if and only if a
 * parameter's validity, freshness or owner-offline flag changed: those are the changes the
 * UART link sends as events, and one the count misses would never be sent. */
typedef struct {
    param_validity_t validity[PARAM_COUNT];
    param_freshness_t freshness[PARAM_COUNT];
    bool owner_offline[PARAM_COUNT];
} reported_t;

static reported_t last_reported;
static uint32_t last_count;
static uint32_t count_checks;

static reported_t reported_now(void)
{
    reported_t r;
    memset(&r, 0, sizeof(r));
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        param_entry_t e;
        (void)params_get(id, 0U, &e);
        r.validity[id - 1U] = e.validity;
        r.freshness[id - 1U] = e.freshness;
        r.owner_offline[id - 1U] = e.owner_offline;
    }
    return r;
}

static void check_count(int line)
{
    reported_t now = reported_now();
    bool changed = false;
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        changed = changed || (now.validity[i] != last_reported.validity[i])
                  || (now.freshness[i] != last_reported.freshness[i])
                  || (now.owner_offline[i] != last_reported.owner_offline[i]);
    }
    uint32_t count = params_change_count();
    if (changed != (count != last_count)) {
        printf("  FAIL line %d: %s, change count %s\n", line, changed ? "state changed" : "nothing changed",
               (count != last_count) ? "moved" : "did not move");
        failures++;
    }
    last_reported = now;
    last_count = count;
    count_checks++;
}

static void watched_init(int line)
{
    params_init();
    if (params_change_count() != 0U) {
        printf("  FAIL line %d: change count not 0 after params_init()\n", line);
        failures++;
    }
    last_reported = reported_now();
    last_count = 0U;
}

static bool watched_set_local(uint8_t id, param_value_t value, uint32_t now, int line)
{
    bool ok = params_set_local(id, value, now);
    check_count(line);
    return ok;
}

static void watched_apply(uint8_t node, const comm_param_block_t *b, uint32_t seq, uint32_t t, int line)
{
    params_apply_block(node, b, seq, t);
    check_count(line);
}

static void watched_freshness(uint32_t now, int line)
{
    params_update_freshness(now);
    check_count(line);
}

static void watched_owner_offline(uint8_t node, bool offline, uint32_t now, int line)
{
    params_set_owner_offline(node, offline, now);
    check_count(line);
}

static void watched_source(uint32_t now, int line)
{
    param_source_task(now); /* Samples through params_set_local() */
    check_count(line);
}

#define params_init()                         watched_init(__LINE__)
#define params_set_local(id, v, now)          watched_set_local((id), (v), (now), __LINE__)
#define params_apply_block(n, b, seq, t)      watched_apply((n), (b), (seq), (t), __LINE__)
#define params_update_freshness(now)          watched_freshness((now), __LINE__)
#define params_set_owner_offline(n, off, now) watched_owner_offline((n), (off), (now), __LINE__)
#define param_source_task(now)                watched_source((now), __LINE__)

static param_entry_t get(uint8_t id, uint32_t now)
{
    param_entry_t e;
    memset(&e, 0xA5, sizeof(e));
    CHECK(params_get(id, now, &e));
    return e;
}

static uint32_t f2u(float f)
{
    param_value_t v = { .f = f };
    return v.u;
}

/* Node 1's block: P1 float, P2 float, P3 int32, P4 uint32, P5 bool, all status OK. */
static comm_param_block_t block1(void)
{
    comm_param_block_t b = { .status = 0U };
    b.values[0] = f2u(1.5f);
    b.values[1] = f2u(-2.25f);
    param_value_t i = { .i = -7 };
    b.values[2] = i.u;
    b.values[3] = 4000000000U;
    b.values[4] = 1U;
    return b;
}

static void test_table(void)
{
    printf("table\n");
    static const param_type_t types[5] = { PARAM_TYPE_FLOAT32, PARAM_TYPE_FLOAT32, PARAM_TYPE_INT32,
                                           PARAM_TYPE_UINT32, PARAM_TYPE_BOOL };
    static const uint32_t periods[5] = { 10U, 50U, 100U, 500U, 1000U };
    char name[16];

    CHECK(system_table_get(0U) == NULL);
    CHECK(system_table_get(21U) == NULL);
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        const param_def_t *d = system_table_get(id);
        uint32_t slot = (id - 1U) % 5U;
        snprintf(name, sizeof(name), "Param %u", (unsigned)id);
        CHECK(d != NULL);
        CHECK(d->id == id);
        CHECK(d->owner == ((id - 1U) / 5U) + 1U);
        CHECK(strcmp(d->name, name) == 0);
        CHECK(d->type == types[slot]);
        CHECK(d->sample_period_ms == periods[slot]);
        CHECK(d->source_stale_ms == 3U * periods[slot]);
    }
}

static void test_init(void)
{
    printf("init\n");
    params_init();
    param_entry_t e;
    CHECK(!params_get(0U, 0U, &e));
    CHECK(!params_get(21U, 0U, &e));
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        e = get(id, 0U);
        CHECK(e.validity == PARAM_NEVER_RECEIVED);
        CHECK(e.freshness == PARAM_STALE);
        CHECK(e.owner == ((id - 1U) / 5U) + 1U);
    }
    CHECK(params_synced_count() == 0U);
}

static void test_receive_and_transport_stale(void)
{
    printf("receive, transport stale at 30 ms\n");
    params_init();
    comm_param_block_t b = block1();

    params_apply_block(1U, &b, 7U, 100U);
    log_clear();
    params_update_freshness(100U);
    param_entry_t e = get(1U, 104U);
    CHECK(e.validity == PARAM_VALID && e.freshness == PARAM_FRESH);
    CHECK(e.value.f == 1.5f && e.seq == 7U && e.last_update_ms == 100U && e.age_ms == 4U);
    CHECK(get(3U, 100U).value.i == -7);
    CHECK(get(4U, 100U).value.u == 4000000000U);
    CHECK(get(5U, 100U).value.u == 1U);
    CHECK(params_synced_count() == 5U);
    CHECK(logged("MCU2 t=100 db VALID 1 2 3 4 5\n"));
    CHECK(logged("MCU2 t=100 db FRESH 1 2 3 4 5\n"));

    log_clear();
    params_update_freshness(130U); /* Age 30: still fresh */
    CHECK(get(1U, 130U).freshness == PARAM_FRESH);
    CHECK(log_buf[0] == '\0'); /* Nothing changed, nothing printed */
    params_update_freshness(131U); /* Age 31 */
    CHECK(get(1U, 131U).freshness == PARAM_STALE);
    CHECK(get(1U, 131U).validity == PARAM_VALID); /* Independent of freshness */
    CHECK(params_synced_count() == 0U);
    CHECK(logged("MCU2 t=131 db STALE 1 2 3 4 5\n"));

    /* Back after 40 ms of silence: stored, but STALE until a block on the beat (settle rule). */
    log_clear();
    params_apply_block(1U, &b, 8U, 140U);
    params_update_freshness(140U);
    CHECK(get(1U, 140U).freshness == PARAM_STALE && get(1U, 140U).seq == 8U);
    CHECK(log_buf[0] == '\0');
    params_apply_block(1U, &b, 9U, 150U);
    params_update_freshness(150U);
    CHECK(get(1U, 150U).freshness == PARAM_FRESH);
    CHECK(logged("MCU2 t=150 db FRESH 1 2 3 4 5\n"));
    CHECK(!logged("VALID")); /* Validity did not change */
}

static void test_settle(void)
{
    printf("settle rule after a silence (open item 2)\n");
    params_init();
    comm_param_block_t old_b = block1(); /* P1 = 1.5: sampled before the outage */
    comm_param_block_t new_b = block1();
    new_b.values[0] = f2u(2.5f);         /* Sampled after it */
    comm_param_block_t b3 = block1();
    params_stats_t s;
    param_entry_t e;

    params_apply_block(1U, &old_b, 1U, 100U);
    params_update_freshness(100U);
    CHECK(get(1U, 100U).freshness == PARAM_FRESH); /* First block ever: no settling */
    params_apply_block(1U, &old_b, 2U, 110U);

    /* Node 1 silent for 1 s, node 3 on the beat. */
    for (uint32_t t = 110U; t <= 1110U; t += 10U) {
        params_apply_block(3U, &b3, t / 10U, t);
    }
    params_update_freshness(1110U);
    CHECK(get(1U, 1110U).freshness == PARAM_STALE);

    /* Reconnect: three blocks flushed from node 1's Tx FIFO, back-to-back, values from 110 ms. */
    params_apply_block(1U, &old_b, 3U, 1110U);
    params_apply_block(1U, &old_b, 4U, 1110U);
    params_apply_block(1U, &old_b, 5U, 1111U);
    log_clear();
    params_update_freshness(1111U);
    params_update_freshness(1115U);
    e = get(1U, 1115U);
    CHECK(e.validity == PARAM_VALID && e.freshness == PARAM_STALE && e.value.f == 1.5f && e.seq == 5U);
    CHECK(!logged("FRESH"));
    CHECK(get(11U, 1115U).freshness == PARAM_FRESH); /* Node 3 is not affected */
    CHECK(params_synced_count() == 5U);
    params_get_stats(&s);
    CHECK(s.held[0] == 3U && s.held[2] == 0U);

    /* The first block sampled after the reconnect, 9 ms later: on the beat. */
    params_apply_block(1U, &new_b, 6U, 1120U);
    params_apply_block(3U, &b3, 112U, 1120U);
    params_update_freshness(1120U);
    e = get(1U, 1120U);
    CHECK(e.freshness == PARAM_FRESH && e.value.f == 2.5f);
    CHECK(logged("MCU2 t=1120 db FRESH 1 2 3 4 5\n"));
    CHECK(params_synced_count() == 10U);

    /* Boundaries: a 30 ms gap is not a silence, 31 is; 4 ms is too close, 5 is on the beat. */
    params_apply_block(1U, &new_b, 7U, 1150U);
    params_update_freshness(1150U);
    CHECK(get(1U, 1150U).freshness == PARAM_FRESH);
    params_apply_block(1U, &new_b, 8U, 1181U);
    params_update_freshness(1181U);
    CHECK(get(1U, 1181U).freshness == PARAM_STALE);
    params_apply_block(1U, &new_b, 9U, 1185U);
    params_update_freshness(1185U);
    CHECK(get(1U, 1185U).freshness == PARAM_STALE);
    params_apply_block(1U, &new_b, 10U, 1190U);
    params_update_freshness(1190U);
    CHECK(get(1U, 1190U).freshness == PARAM_FRESH);
    params_get_stats(&s);
    CHECK(s.held[0] == 5U);
}

static void test_owner_offline(void)
{
    printf("owner offline flag, node synced (for diag/)\n");
    params_init();
    comm_param_block_t b = block1();

    CHECK(!params_node_synced(1U));
    params_apply_block(1U, &b, 1U, 100U);
    params_update_freshness(100U);
    CHECK(params_node_synced(1U));
    CHECK(!params_node_synced(2U) && !params_node_synced(0U) && !params_node_synced(5U));
    CHECK(!get(1U, 100U).owner_offline);

    log_clear();
    params_set_owner_offline(1U, true, 120U);
    CHECK(get(1U, 120U).owner_offline && get(5U, 120U).owner_offline);
    CHECK(!get(6U, 120U).owner_offline && !get(11U, 120U).owner_offline);
    CHECK(logged("MCU2 t=120 db OWNER_OFFLINE 1 2 3 4 5\n"));
    CHECK(get(1U, 120U).validity == PARAM_VALID && get(1U, 120U).value.f == 1.5f); /* Kept */

    log_clear();
    params_set_owner_offline(1U, true, 125U); /* No change, no line */
    params_set_owner_offline(2U, true, 125U); /* This node: ignored */
    params_set_owner_offline(5U, true, 125U);
    CHECK(log_buf[0] == '\0' && !get(6U, 125U).owner_offline);
    params_set_owner_offline(1U, false, 130U);
    CHECK(!get(1U, 130U).owner_offline);
    CHECK(logged("MCU2 t=130 db OWNER_ONLINE 1 2 3 4 5\n"));

    params_update_freshness(131U); /* Age 31: stale, so no longer synced */
    CHECK(!params_node_synced(1U));
}

static void test_ignored_blocks(void)
{
    printf("own and unknown senders ignored\n");
    params_init();
    comm_param_block_t b = block1();

    params_apply_block(2U, &b, 1U, 100U); /* Our own parameters */
    params_apply_block(0U, &b, 1U, 100U);
    params_apply_block(5U, &b, 1U, 100U);
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        CHECK(get(id, 100U).validity == PARAM_NEVER_RECEIVED);
    }
    params_stats_t s;
    params_get_stats(&s);
    CHECK(s.blocks[0] == 0U && s.blocks[1] == 0U && s.blocks[2] == 0U && s.blocks[3] == 0U);
}

static void test_check8(void)
{
    printf("check 8: source status, NaN, infinity, bool, full ranges\n");
    params_init();
    comm_param_block_t good = block1();
    params_apply_block(1U, &good, 1U, 100U);

    comm_param_block_t b = block1();
    b.values[0] = f2u(NAN);               /* P1: rejected */
    b.status = (uint16_t)((2U << 2)       /* P2: owner says SOURCE_FAULT */
                          | (1U << 4)     /* P3: owner says SOURCE_STALE */
                          | (3U << 6));   /* P4: reserved code */
    b.values[1] = f2u(9.0f);
    b.values[2] = 99U;
    b.values[4] = 2U;                     /* P5: bool 2 */
    params_apply_block(1U, &b, 2U, 110U);
    params_update_freshness(110U);

    param_entry_t e = get(1U, 110U);
    CHECK(e.validity == PARAM_INVALID && e.value.f == 1.5f); /* Old value kept */
    CHECK(e.last_update_ms == 110U && e.seq == 2U);          /* Timestamp still moves on */
    CHECK(e.freshness == PARAM_FRESH);                       /* INVALID + FRESH (Het, 11 Nov) */
    e = get(2U, 110U);
    CHECK(e.validity == PARAM_INVALID && e.value.f == -2.25f && e.source_status == PARAM_SOURCE_FAULT);
    e = get(3U, 110U);
    CHECK(e.validity == PARAM_VALID && e.value.u == 99U && e.freshness == PARAM_STALE);
    CHECK(get(4U, 110U).validity == PARAM_INVALID);
    CHECK(get(5U, 110U).validity == PARAM_INVALID && get(5U, 110U).value.u == 1U);
    CHECK(params_value_rejected() == 3U); /* NaN, reserved code, bool 2; not SOURCE_FAULT */
    CHECK(params_synced_count() == 0U);

    b = block1();
    b.values[0] = f2u(INFINITY);
    b.values[1] = f2u(-INFINITY);
    params_apply_block(1U, &b, 3U, 120U);
    CHECK(get(1U, 120U).validity == PARAM_INVALID && get(2U, 120U).validity == PARAM_INVALID);
    CHECK(params_value_rejected() == 5U);

    /* Ranges are each type's full domain for now. */
    b.values[0] = f2u(FLT_MAX);
    b.values[1] = f2u(-FLT_MAX);
    param_value_t i = { .i = INT32_MIN };
    b.values[2] = i.u;
    b.values[3] = UINT32_MAX;
    b.values[4] = 0U;
    params_apply_block(1U, &b, 4U, 130U);
    params_update_freshness(130U);
    for (uint8_t id = 1U; id <= 5U; id++) {
        CHECK(get(id, 130U).validity == PARAM_VALID);
    }
    CHECK(get(3U, 130U).value.i == INT32_MIN);
    CHECK(params_value_rejected() == 5U);
}

static void test_local(void)
{
    printf("own parameters: set, source stale, fault, block\n");
    params_init();
    param_value_t v = { .f = 3.0f };

    CHECK(!params_set_local(1U, v, 0U));  /* Node 1 owns P1 */
    CHECK(!params_set_local(21U, v, 0U));
    CHECK(params_set_local(6U, v, 200U));
    params_update_freshness(200U);
    param_entry_t e = get(6U, 200U);
    CHECK(e.validity == PARAM_VALID && e.freshness == PARAM_FRESH && e.seq == 1U);

    comm_param_block_t b;
    params_build_block(&b);
    CHECK(b.values[0] == f2u(3.0f));
    CHECK((b.status & 0x3U) == PARAM_SOURCE_OK);
    CHECK(((b.status >> 2) & 0x3U) == PARAM_SOURCE_STALE); /* P7 not sampled yet */

    params_update_freshness(230U); /* P6: 10 ms period, stale after 30 */
    CHECK(get(6U, 230U).freshness == PARAM_FRESH);
    params_update_freshness(231U);
    CHECK(get(6U, 231U).freshness == PARAM_STALE);
    params_build_block(&b);
    CHECK((b.status & 0x3U) == PARAM_SOURCE_STALE);

    param_value_t bad = { .f = NAN };
    CHECK(!params_set_local(6U, bad, 240U));
    params_update_freshness(240U);
    e = get(6U, 240U);
    CHECK(e.validity == PARAM_INVALID && e.value.f == 3.0f && e.seq == 2U && e.freshness == PARAM_FRESH);
    params_build_block(&b);
    CHECK((b.status & 0x3U) == PARAM_SOURCE_FAULT);
    CHECK(b.values[0] == f2u(3.0f));
    CHECK(params_value_rejected() == 0U); /* Own faults are reported in the status, not counted */
}

static void test_gaps_and_wrap(void)
{
    printf("arrival gaps, ms counter wrap\n");
    params_init();
    comm_param_block_t b = block1();
    params_stats_t s;

    params_apply_block(3U, &b, 1U, 0xFFFFFFF0U);
    params_apply_block(3U, &b, 2U, 0xFFFFFFFAU);
    params_apply_block(3U, &b, 3U, 0x00000009U); /* 15 ms later, across the wrap */
    params_get_stats(&s);
    CHECK(s.blocks[2] == 3U && s.last_gap_ms[2] == 15U && s.worst_gap_ms[2] == 15U);

    params_update_freshness(0x00000027U); /* Age 30 across the wrap */
    CHECK(get(11U, 0x27U).freshness == PARAM_FRESH);
    params_update_freshness(0x00000028U);
    CHECK(get(11U, 0x28U).freshness == PARAM_STALE);
}

static void test_source(void)
{
    printf("simulated sensors (node 2)\n");
    params_init();

    param_source_task(0U);
    for (uint8_t id = 6U; id <= 10U; id++) {
        CHECK(get(id, 0U).validity == PARAM_VALID);
    }
    CHECK(get(8U, 0U).value.i == -1000);
    CHECK(get(9U, 0U).value.u == 0U);
    CHECK(get(10U, 0U).value.u == 0U);
    for (uint32_t t = 10U; t <= 1000U; t += 10U) {
        param_source_task(t);
        CHECK(fabsf(get(6U, t).value.f) <= 50.0f);              /* ±25 x node 2 */
        float f7 = get(7U, t).value.f;
        CHECK((f7 >= 150.0f) && (f7 <= 350.0f));                /* 250 ± 50 x node 2 */
    }
    CHECK(get(6U, 1000U).seq == 101U);                          /* Every 10 ms */
    CHECK(get(7U, 1000U).seq == 21U);                           /* Every 50 ms */
    CHECK(get(8U, 1000U).seq == 11U && get(8U, 1000U).value.i == -800); /* 10 x 20 steps */
    CHECK(get(9U, 1000U).seq == 3U && get(9U, 1000U).value.u == 400U);
    CHECK(get(10U, 1000U).seq == 2U && get(10U, 1000U).value.u == 1U);

    /* The int ramp wraps from 1000 back to -1000: 100 steps of 20 from -1000. */
    for (uint32_t t = 1010U; t <= 11000U; t += 10U) {
        param_source_task(t);
        int32_t i = get(8U, t).value.i;
        CHECK((i >= -1000) && (i <= 1000));
    }
    /* 111 samples (0 to 11000 ms): one lap of 101 values (-1000 to 1000), then 10 more. */
    CHECK(get(8U, 11000U).value.i == -820);
}

int main(void)
{
    test_table();
    test_init();
    test_receive_and_transport_stale();
    test_settle();
    test_owner_offline();
    test_ignored_blocks();
    test_check8();
    test_local();
    test_gaps_and_wrap();
    test_source();
    printf("change count checked after %u params/ calls\n", (unsigned)count_checks);
    printf("%s (%d failures)\n", (failures == 0) ? "PARAMS PASS" : "PARAMS FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
