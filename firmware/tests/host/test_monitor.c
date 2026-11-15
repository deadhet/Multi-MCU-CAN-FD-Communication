/* PC test for the UART link: monitor/monitor.c, monitor/cobs.c, util/crc16.c, drivers/tx_slots.c
 * and nanopb, built as node 1. The USART is replaced by a capture buffer that the test empties
 * the slots into, or not, to play a busy link. Frames are decoded back here with nanopb,
 * and written to build/host/monitor_frames.bin for the Python decoder test
 * (dashboard/tests/test_framing.py), which checks the same values.
 *   firmware/tests/host/run_host_tests.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor/cobs.h"
#include "monitor/monitor.h"
#include "monitor/monitor.pb.h"
#include "drivers/fdcan.h"
#include "drivers/uart_link.h"
#include "util/crc16.h"
#include "pb_decode.h"

#if NODE_ID != 1
#error "test_monitor.c expects NODE_ID=1"
#endif

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* ---- Stand-ins for the drivers: the real slots, a capture buffer as the USART ---- */
static uint8_t captured[16384];
static uint32_t captured_len;
static bool link_busy; /* True: the mover does not run, frames wait in the slots */

/* The mover: every pending frame, oldest hand-over first, into the capture buffer. */
static void drain(void)
{
    const uint8_t *data = NULL;
    uint32_t len = 0U;

    while (tx_slots_next(&data, &len)) {
        if (captured_len + len <= sizeof(captured)) {
            memcpy(&captured[captured_len], data, len);
            captured_len += len;
        }
        tx_slots_done();
    }
}

void uart_link_attach(tx_slot_t slot, uint8_t *buf_a, uint8_t *buf_b, uint32_t size)
{
    tx_slots_attach(slot, buf_a, buf_b, size);
}

uint8_t *uart_link_begin(tx_slot_t slot, uint32_t *size, bool *replaced)
{
    return tx_slots_begin(slot, size, replaced);
}

void uart_link_commit(tx_slot_t slot, uint32_t len)
{
    tx_slots_commit(slot, len);
    if (!link_busy) {
        drain();
    }
}

void uart_link_attach_queue(uint8_t *entries, uint32_t entry_size)
{
    tx_slots_attach_queue(entries, entry_size);
}

uint8_t *uart_link_queue_begin(uint32_t *size) { return tx_slots_queue_begin(size); }

void uart_link_queue_commit(uint32_t len)
{
    tx_slots_queue_commit(len);
    if (!link_busy) {
        drain();
    }
}

void uart_link_get_stats(tx_slots_stats_t *out) { tx_slots_get_stats(out); }

/* As uart_link_init() then monitor_init() on MCU1. */
static void start_link(void)
{
    tx_slots_init();
    monitor_init();
    captured_len = 0U;
    link_busy = false;
}
uint32_t fdcan_nominal_bitrate(void) { return 500000U; }
uint32_t fdcan_data_bitrate(void) { return 2000000U; }
bool fdcan_brs_enabled(void) { return true; }

/* ---- COBS decoder, for the checks ---- */
static int cobs_decode(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t *out_len)
{
    uint32_t i = 0U, o = 0U;

    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0U) {
            return 0;
        }
        for (uint8_t k = 1U; k < code; k++) {
            if (i >= len) {
                return 0;
            }
            out[o++] = in[i++];
        }
        if ((code < 0xFFU) && (i < len)) {
            out[o++] = 0U;
        }
    }
    *out_len = o;
    return 1;
}

/* Splits the frame starting at captured[*pos], checks COBS, CRC and header, returns the payload. */
static int take_frame(uint32_t *pos, uint8_t *type, uint32_t *seq, uint8_t *payload, uint32_t *payload_len)
{
    uint32_t start = *pos, end = start;
    uint8_t raw[4096];
    uint32_t raw_len = 0U;

    while ((end < captured_len) && (captured[end] != 0U)) {
        end++;
    }
    if (end >= captured_len) {
        return 0;
    }
    *pos = end + 1U;
    if (!cobs_decode(&captured[start], end - start, raw, &raw_len) || (raw_len < 7U)) {
        return 0;
    }
    uint16_t crc = (uint16_t)(raw[raw_len - 2U] | (raw[raw_len - 1U] << 8));
    if (crc != crc16_update(CRC16_INIT, raw, raw_len - 2U)) {
        return 0;
    }
    if ((raw[0] >> 4) != MONITOR_PROTOCOL_VERSION) {
        return 0;
    }
    *type = raw[0] & 0x0FU;
    *seq = (uint32_t)raw[1] | ((uint32_t)raw[2] << 8) | ((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 24);
    *payload_len = raw_len - 7U;
    memcpy(payload, &raw[5], *payload_len);
    return 1;
}

static void test_cobs(void)
{
    printf("COBS: known vectors, round trip, no 0x00 in the output\n");
    static const struct { uint8_t in[8]; uint32_t len; uint8_t out[9]; uint32_t out_len; } v[] = {
        { { 0 }, 0, { 0x01 }, 1 },
        { { 0x00 }, 1, { 0x01, 0x01 }, 2 },
        { { 0x00, 0x00 }, 2, { 0x01, 0x01, 0x01 }, 3 },
        { { 0x11, 0x22, 0x00, 0x33 }, 4, { 0x03, 0x11, 0x22, 0x02, 0x33 }, 5 },
        { { 0x11, 0x00, 0x00, 0x00 }, 4, { 0x02, 0x11, 0x01, 0x01, 0x01 }, 5 },
    };
    uint8_t out[1100], back[1100];
    uint32_t back_len;

    for (uint32_t k = 0U; k < sizeof(v) / sizeof(v[0]); k++) {
        uint32_t n = cobs_encode(v[k].in, v[k].len, out);
        CHECK(n == v[k].out_len && memcmp(out, v[k].out, n) == 0);
    }

    /* 254 non-zero bytes fill one group exactly; 255 need a second group. */
    uint8_t in[1024];
    for (uint32_t i = 0U; i < sizeof(in); i++) {
        in[i] = (uint8_t)(1U + (i % 255U));
    }
    uint32_t n = cobs_encode(in, 254U, out);
    CHECK(n == 256U && out[0] == 0xFFU && out[255] == 0x01U);
    n = cobs_encode(in, 255U, out);
    CHECK(n == 257U && out[0] == 0xFFU && out[255] == 0x02U);

    srand(1);
    for (uint32_t round = 0U; round < 500U; round++) {
        uint32_t len = (uint32_t)rand() % 1000U;
        for (uint32_t i = 0U; i < len; i++) {
            in[i] = ((rand() % 4) == 0) ? 0U : (uint8_t)rand();
        }
        n = cobs_encode(in, len, out);
        CHECK(n <= COBS_MAX_ENCODED(len));
        CHECK(memchr(out, 0, n) == NULL);
        CHECK(cobs_decode(out, n, back, &back_len) && back_len == len && memcmp(back, in, len) == 0);
    }
}

static param_entry_t param(uint8_t id, param_type_t type, param_value_t v, param_validity_t val,
                           param_freshness_t fr, uint32_t age, uint32_t seq)
{
    param_entry_t e = { .id = id, .owner = (uint8_t)(((id - 1U) / 5U) + 1U), .type = type, .value = v,
                        .seq = seq, .age_ms = age, .validity = val, .freshness = fr };
    return e;
}

/* The values dashboard/tests/test_framing.py also expects. */
static void make_view(monitor_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->now_ms = 1005U;
    v->own_state = COMM_STATE_OPERATIONAL;
    v->synced_count = 12U;
    v->own_error_summary = 2U;
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        const param_def_t *d = system_table_get(id);
        v->params[id - 1U] = param(id, d->type, (param_value_t){ .u = 0U }, PARAM_NEVER_RECEIVED, PARAM_STALE, 1005U, 0U);
    }
    v->params[0] = param(1, PARAM_TYPE_FLOAT32, (param_value_t){ .f = 1.5f }, PARAM_VALID, PARAM_FRESH, 3U, 7U);
    v->params[1] = param(2, PARAM_TYPE_FLOAT32, (param_value_t){ .f = -2.25f }, PARAM_VALID, PARAM_FRESH, 3U, 7U);
    v->params[2] = param(3, PARAM_TYPE_INT32, (param_value_t){ .i = -910 }, PARAM_VALID, PARAM_FRESH, 3U, 7U);
    v->params[3] = param(4, PARAM_TYPE_UINT32, (param_value_t){ .u = 4000000000U }, PARAM_VALID, PARAM_FRESH, 3U, 7U);
    v->params[4] = param(5, PARAM_TYPE_BOOL, (param_value_t){ .u = 1U }, PARAM_VALID, PARAM_FRESH, 3U, 7U);
    v->params[5] = param(6, PARAM_TYPE_FLOAT32, (param_value_t){ .u = 0x7FC00000U }, PARAM_INVALID, PARAM_FRESH, 4U, 99U);
    v->params[10] = param(11, PARAM_TYPE_FLOAT32, (param_value_t){ .f = 296.716f }, PARAM_VALID, PARAM_FRESH, 5U, 1500U);
    v->params[15] = param(16, PARAM_TYPE_FLOAT32, (param_value_t){ .f = 12.0f }, PARAM_VALID, PARAM_STALE, 1205U, 88U);
    v->params[15].source_status = (uint8_t)PARAM_SOURCE_STALE;
    v->params[15].owner_offline = true;

    v->peers[1] = (diag_peer_t){ .link = DIAG_ONLINE, .heard = true,
                                 .last = { .state = COMM_STATE_OPERATIONAL, .synced_count = 20U, .error_summary = 0U },
                                 .heartbeat_age_ms = 5U, .data_age_ms = 3U };
    v->peers[2] = (diag_peer_t){ .link = DIAG_OFFLINE, .heard = true,
                                 .last = { .state = COMM_STATE_OPERATIONAL, .synced_count = 20U, .error_summary = 7U },
                                 .heartbeat_age_ms = 1205U, .missed = 11U, .restarts = 1U, .recoveries = 1U, .data_age_ms = 1210U };
    v->peers[3] = (diag_peer_t){ .link = DIAG_UNKNOWN, .heard = false, .heartbeat_age_ms = 300U, .data_age_ms = 300U };
}

static int contains(const uint8_t *hay, uint32_t n, const uint8_t *needle, uint32_t m)
{
    for (uint32_t i = 0U; i + m <= n; i++) {
        if (memcmp(&hay[i], needle, m) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_snapshot(void)
{
    printf("SNAPSHOT: frame, CRC, header, every field decoded back, P11 bytes as worked out by hand\n");
    monitor_view_t view;
    make_view(&view);
    start_link();
    CHECK(monitor_send_snapshot(&view));
    CHECK(captured_len > 0U && captured[captured_len - 1U] == 0U);

    uint32_t pos = 0U, seq = 0U, len = 0U;
    uint8_t type = 0U, payload[2048];
    CHECK(take_frame(&pos, &type, &seq, payload, &len));
    CHECK(type == MONITOR_FRAME_SNAPSHOT && seq == 1U && pos == captured_len);
    printf("  snapshot: %u payload bytes, %u on the wire (max payload %u)\n", (unsigned)len,
           (unsigned)captured_len, (unsigned)monitor_Snapshot_size);

    /* P11 as worked out by hand: sub-message tag 0x1A, length 0x12, then 18 bytes. */
    float f = 296.716f;
    uint8_t fb[4];
    memcpy(fb, &f, 4);
    const uint8_t p11[] = { 0x1A, 0x12, 0x08, 0x0B, 0x10, 0x03, 0x1D, fb[0], fb[1], fb[2], fb[3],
                            0x38, 0x01, 0x40, 0x01, 0x48, 0x05, 0x50, 0xDC, 0x0B };
    CHECK(contains(payload, len, p11, sizeof(p11)));

    monitor_Snapshot s = monitor_Snapshot_init_zero;
    pb_istream_t in = pb_istream_from_buffer(payload, len);
    CHECK(pb_decode(&in, monitor_Snapshot_fields, &s));
    CHECK(s.uptime_ms == 1005U && s.nodes_count == 4U && s.params_count == 20U);
    CHECK(s.online_count == 2U && s.synced_count == 12U && s.frames_skipped == 0U);
    CHECK(s.health_percent == 50U); /* The weaker of 2/4 (50 %) and 12/20 (60 %) */

    CHECK(s.nodes[0].node == 1U && s.nodes[0].link == monitor_LinkState_SELF);
    CHECK(s.nodes[0].reported_state == monitor_NodeState_OPERATIONAL && s.nodes[0].synced_count == 12U && s.nodes[0].error_summary == 2U);
    CHECK(s.nodes[1].link == monitor_LinkState_ONLINE && s.nodes[1].heartbeat_age_ms == 5U && s.nodes[1].synced_count == 20U);
    CHECK(s.nodes[2].link == monitor_LinkState_OFFLINE && s.nodes[2].missed == 11U && s.nodes[2].restarts == 1U);
    CHECK(s.nodes[2].recoveries == 1U && s.nodes[2].error_summary == 7U && s.nodes[2].data_age_ms == 1210U);
    CHECK(s.nodes[3].link == monitor_LinkState_UNKNOWN && s.nodes[3].reported_state == monitor_NodeState_BOOT);

    CHECK(s.params[0].which_value == monitor_Parameter_f32_tag && s.params[0].value.f32 == 1.5f);
    CHECK(s.params[1].value.f32 == -2.25f);
    CHECK(s.params[2].which_value == monitor_Parameter_i32_tag && s.params[2].value.i32 == -910);
    CHECK(s.params[3].which_value == monitor_Parameter_u32_tag && s.params[3].value.u32 == 4000000000U);
    CHECK(s.params[4].which_value == monitor_Parameter_flag_tag && s.params[4].value.flag);
    CHECK(s.params[0].validity == monitor_Validity_VALID && s.params[0].freshness == monitor_Freshness_FRESH);
    CHECK(s.params[0].age_ms == 3U && s.params[0].sequence == 7U && s.params[0].source == 1U);
    CHECK(isnan(s.params[5].value.f32) && s.params[5].validity == monitor_Validity_INVALID);
    CHECK(s.params[10].value.f32 == 296.716f && s.params[10].sequence == 1500U && s.params[10].source == 3U);
    CHECK(s.params[15].freshness == monitor_Freshness_STALE && s.params[15].owner_offline);
    CHECK(s.params[15].source_status == monitor_SourceStatus_SOURCE_STALE && s.params[15].age_ms == 1205U);
    CHECK(s.params[19].id == 20U && s.params[19].validity == monitor_Validity_NEVER_RECEIVED);
    CHECK(s.params[19].which_value == monitor_Parameter_flag_tag && !s.params[19].value.flag);
}

static void test_config(void)
{
    printf("CONFIG: bit rates, periods, the 20 names, owners and sample periods\n");
    const monitor_periods_t periods = { 10U, 100U, 100U };
    uint32_t start = captured_len;
    CHECK(monitor_send_config(&periods));

    uint32_t pos = start, seq = 0U, len = 0U;
    uint8_t type = 0U, payload[2048];
    CHECK(take_frame(&pos, &type, &seq, payload, &len));
    CHECK(type == MONITOR_FRAME_CONFIG && seq == 2U);
    printf("  config: %u payload bytes (max %u)\n", (unsigned)len, (unsigned)monitor_Config_size);

    monitor_Config c = monitor_Config_init_zero;
    pb_istream_t in = pb_istream_from_buffer(payload, len);
    CHECK(pb_decode(&in, monitor_Config_fields, &c));
    CHECK(c.protocol_version == 1U && strcmp(c.firmware, "1.0-dev") == 0);
    CHECK(c.arbitration_bps == 500000U && c.data_bps == 2000000U && c.brs);
    CHECK(c.node_count == 4U && c.param_count == 20U && c.params_count == 20U);
    CHECK(c.param_period_ms == 10U && c.heartbeat_period_ms == 100U && c.snapshot_period_ms == 100U);
    CHECK(strcmp(c.params[0].name, "Param 1") == 0 && strcmp(c.params[19].name, "Param 20") == 0);
    CHECK(c.params[10].owner == 3U && c.params[4].sample_period_ms == 1000U && c.params[3].sample_period_ms == 500U);
}

/* Decodes a snapshot payload; returns its uptime and sets *skipped, or returns 0xFFFFFFFF. */
static uint32_t snapshot_uptime(const uint8_t *payload, uint32_t len, uint32_t *skipped)
{
    monitor_Snapshot s = monitor_Snapshot_init_zero;
    pb_istream_t in = pb_istream_from_buffer(payload, len);

    if (!pb_decode(&in, monitor_Snapshot_fields, &s)) {
        return 0xFFFFFFFFU;
    }
    *skipped = s.frames_skipped;
    return s.uptime_ms;
}

static void test_latest_wins(void)
{
    printf("Latest wins (§8.4): a busy link keeps only the newest frame of each kind; gaps = frames_skipped\n");
    monitor_view_t view;
    const monitor_periods_t periods = { 10U, 100U, 100U };
    uint32_t pos = 0U, seq = 0U, len = 0U, skipped = 0U;
    uint8_t type = 0U, payload[2048];
    monitor_stats_t st;

    make_view(&view);
    start_link();
    link_busy = true;
    view.now_ms = 100U;
    CHECK(monitor_send_snapshot(&view)); /* seq 1 */
    CHECK(monitor_send_config(&periods)); /* seq 2 */
    view.now_ms = 200U;
    CHECK(monitor_send_snapshot(&view)); /* seq 3: replaces 1 */
    view.now_ms = 300U;
    CHECK(monitor_send_snapshot(&view)); /* seq 4: replaces 3 */
    CHECK(captured_len == 0U);
    monitor_get_stats(&st);
    CHECK(st.snapshots == 3U && st.configs == 1U && st.skipped == 2U);

    /* The link frees up: CONFIG first (handed over first), then only the newest snapshot. It
     * already counts the two replaced ones, which are exactly the sequence gap (1 and 3). */
    link_busy = false;
    drain();
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && type == MONITOR_FRAME_CONFIG && seq == 2U);
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && type == MONITOR_FRAME_SNAPSHOT && seq == 4U);
    CHECK(snapshot_uptime(payload, len, &skipped) == 300U && skipped == 2U);
    CHECK(pos == captured_len);

    /* A snapshot made while the previous one is on its way out goes to the slot's other buffer:
     * the one being sent is not touched, and nothing is replaced. */
    captured_len = 0U;
    link_busy = true;
    view.now_ms = 400U;
    CHECK(monitor_send_snapshot(&view)); /* seq 5 */
    const uint8_t *out = NULL;
    uint32_t out_len = 0U;
    CHECK(tx_slots_next(&out, &out_len)); /* The mover starts seq 5... */
    static uint8_t copy[2048];
    memcpy(copy, out, out_len);
    view.now_ms = 500U;
    CHECK(monitor_send_snapshot(&view)); /* seq 6, while 5 is being sent */
    CHECK(memcmp(out, copy, out_len) == 0);
    memcpy(captured, out, out_len); /* ...and finishes it */
    captured_len = out_len;
    tx_slots_done();
    drain();
    pos = 0U;
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && seq == 5U);
    CHECK(snapshot_uptime(payload, len, &skipped) == 400U && skipped == 2U);
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && seq == 6U);
    CHECK(snapshot_uptime(payload, len, &skipped) == 500U && skipped == 2U);
    monitor_get_stats(&st);
    CHECK(st.skipped == 2U && st.encode_errors == 0U);
    link_busy = false;
}

static void test_health(void)
{
    printf("Health (option b): the weaker of online/4 and synced/20, whole percent, rounded down\n");
    static const struct { uint32_t online, synced, health; } cases[] = {
        { 4U, 20U, 100U }, /* all fine */
        { 3U, 15U, 75U },  /* one controller dead, its 5 parameters with it */
        { 4U, 15U, 75U },  /* all online, one sensor faulty */
        { 4U, 5U, 25U },   /* just booted */
        { 4U, 0U, 0U },    /* all talking, no data gets through: 0, not 50 */
        { 4U, 19U, 95U },
        { 1U, 20U, 25U },  /* alone */
        { 4U, 1U, 5U },
        { 3U, 19U, 75U },
        { 0U, 0U, 0U },
    };
    for (uint32_t k = 0U; k < sizeof(cases) / sizeof(cases[0]); k++) {
        CHECK(monitor_health_percent(cases[k].online, cases[k].synced) == cases[k].health);
    }

    /* Online: this node always, peers only when ONLINE or LATE. */
    monitor_view_t view;
    make_view(&view); /* n2 ONLINE, n3 OFFLINE, n4 UNKNOWN */
    CHECK(monitor_online_count(&view) == 2U);
    view.peers[3].link = DIAG_ONLINE;
    CHECK(monitor_online_count(&view) == 3U);
    view.peers[1].link = DIAG_LATE; /* LATE still counts */
    view.peers[2].link = DIAG_DATA_INVALID;
    CHECK(monitor_online_count(&view) == 3U);
    view.peers[1].link = DIAG_RECOVERING;
    CHECK(monitor_online_count(&view) == 2U);
    view.peers[1].link = DIAG_NO_HEARTBEAT;
    CHECK(monitor_online_count(&view) == 2U);
    view.peers[0].link = DIAG_ONLINE; /* The entry for this node is ignored */
    CHECK(monitor_online_count(&view) == 2U);
}

static void test_worst_case(void)
{
    printf("Worst case: every field at its largest still fits the generator's maximum\n");
    monitor_view_t view;
    make_view(&view);
    view.now_ms = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        view.params[i].age_ms = 0xFFFFFFFFU;
        view.params[i].seq = 0xFFFFFFFFU;
        view.params[i].validity = PARAM_INVALID;
        view.params[i].freshness = PARAM_FRESH;
        view.params[i].source_status = 3U;
        view.params[i].owner_offline = true;
        view.params[i].value.u = 0xFFFFFFFFU;
    }
    for (uint32_t n = 1U; n < COMM_NODE_COUNT; n++) {
        view.peers[n] = (diag_peer_t){ .link = DIAG_DATA_INVALID, .heard = true,
                                       .last = { .state = COMM_STATE_OPERATIONAL, .synced_count = 255U, .error_summary = 0xFFFFU },
                                       .heartbeat_age_ms = 0xFFFFFFFFU, .missed = 0xFFFFFFFFU,
                                       .restarts = 0xFFFFFFFFU, .recoveries = 0xFFFFFFFFU, .data_age_ms = 0xFFFFFFFFU };
    }
    start_link();
    CHECK(monitor_send_snapshot(&view));
    uint32_t pos = 0U, seq = 0U, len = 0U;
    uint8_t type = 0U, payload[2048];
    CHECK(take_frame(&pos, &type, &seq, payload, &len));
    CHECK(len <= monitor_Snapshot_size);
    printf("  worst snapshot: %u payload bytes, %u on the wire\n", (unsigned)len, (unsigned)captured_len);
}

/* The next frame must be an EVENT; returns it decoded. */
static monitor_Event next_event(uint32_t *pos)
{
    monitor_Event e = monitor_Event_init_zero;
    uint32_t seq = 0U, len = 0U;
    uint8_t type = 0U, payload[64];

    CHECK(take_frame(pos, &type, &seq, payload, &len) && type == MONITOR_FRAME_EVENT);
    pb_istream_t in = pb_istream_from_buffer(payload, len);
    CHECK(pb_decode(&in, monitor_Event_fields, &e));
    return e;
}

static int is_node_event(const monitor_Event *e, uint32_t nodes, bool restarted, monitor_LinkState link)
{
    if ((e->which_body != monitor_Event_node_tag) || (e->body.node.nodes != nodes)) {
        return 0;
    }
    if (restarted) {
        return (e->body.node.which_change == monitor_NodeEvent_restarted_tag) && e->body.node.change.restarted;
    }
    return (e->body.node.which_change == monitor_NodeEvent_link_tag) && (e->body.node.change.link == link);
}

static int is_param_event(const monitor_Event *e, monitor_ParamChange change, uint32_t params)
{
    return (e->which_body == monitor_Event_param_tag) && (e->body.param.change == change)
        && (e->body.param.params == params);
}

static void test_events(void)
{
    printf("EVENTS: changes since the boot state, grouped by kind, in order; nothing twice; queue full\n");
    monitor_view_t view;
    uint32_t pos = 0U;

    /* From the boot state to make_view(): n3 restarted once, n2 ONLINE, n3 OFFLINE, own state
     * OPERATIONAL; P16's owner offline; P1-P5, P11, P16 VALID; P6 INVALID; P1-P6, P11 FRESH. */
    make_view(&view);
    start_link();
    CHECK(monitor_send_events(&view) == 8U);
    monitor_Event e = next_event(&pos);
    CHECK(e.time_ms == 1005U && is_node_event(&e, 0x4U, true, 0));
    e = next_event(&pos);
    CHECK(is_node_event(&e, 0x2U, false, monitor_LinkState_ONLINE));
    e = next_event(&pos);
    CHECK(is_node_event(&e, 0x4U, false, monitor_LinkState_OFFLINE));
    e = next_event(&pos);
    CHECK(e.which_body == monitor_Event_own_state_tag && e.body.own_state == monitor_NodeState_OPERATIONAL);
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_OWNER_WENT_OFFLINE, 0x8000U));
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_BECAME_VALID, 0x841FU));
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_BECAME_INVALID, 0x20U));
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_BECAME_FRESH, 0x43FU));
    CHECK(pos == captured_len);

    /* Same view again: nothing changed, nothing sent. */
    CHECK(monitor_send_events(&view) == 0U && pos == captured_len);

    /* n2 and n4 go OFFLINE in the same run: one event naming both. n3 restarts again and is
     * RECOVERING; P16's owner is back and P16 FRESH. */
    view.now_ms = 1010U;
    view.peers[1].link = DIAG_OFFLINE;
    view.peers[3].link = DIAG_OFFLINE;
    view.peers[2].link = DIAG_RECOVERING;
    view.peers[2].restarts = 2U;
    view.params[15].owner_offline = false;
    view.params[15].freshness = PARAM_FRESH;
    CHECK(monitor_send_events(&view) == 5U);
    e = next_event(&pos);
    CHECK(e.time_ms == 1010U && is_node_event(&e, 0x4U, true, 0));
    e = next_event(&pos); /* Link events in link-state order: OFFLINE (3) before RECOVERING (4) */
    CHECK(is_node_event(&e, 0xAU, false, monitor_LinkState_OFFLINE));
    e = next_event(&pos);
    CHECK(is_node_event(&e, 0x4U, false, monitor_LinkState_RECOVERING));
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_OWNER_BACK, 0x8000U));
    e = next_event(&pos);
    CHECK(is_param_event(&e, monitor_ParamChange_BECAME_FRESH, 0x8000U));
    CHECK(pos == captured_len);

    /* Queue full: the link is busy and 20 events come in one run. 16 wait, 4 are lost; their
     * sequence numbers are used up and counted, so the next snapshot explains the gap. */
    captured_len = 0U;
    pos = 0U;
    link_busy = true;
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        view.params[i].validity = PARAM_INVALID; /* one change per parameter: still one event */
    }
    CHECK(monitor_send_events(&view) == 1U);
    monitor_stats_t st;
    monitor_get_stats(&st);
    uint32_t seq_before = 13U + 1U; /* 13 events so far, this one is 14 */
    for (uint32_t k = 0U; k < 19U; k++) {
        view.now_ms += 5U;
        view.own_state = (k % 2U == 0U) ? COMM_STATE_RESYNCING : COMM_STATE_OPERATIONAL;
        CHECK(monitor_send_events(&view) == 1U);
    }
    monitor_get_stats(&st);
    CHECK(st.events == 13U + TX_QUEUE_ENTRIES && st.events_dropped == 4U && st.skipped == 4U);
    view.now_ms += 5U;
    CHECK(monitor_send_snapshot(&view)); /* seq 14 + 20 = 34 */
    link_busy = false;
    drain();
    uint32_t seq = 0U, len = 0U, last = 0U, skipped = 0U;
    uint8_t type = 0U, payload[2048];
    uint32_t events_seen = 0U;
    while (take_frame(&pos, &type, &seq, payload, &len)) {
        if (type == MONITOR_FRAME_EVENT) {
            CHECK(seq == seq_before + events_seen); /* the 16 kept: 14-29, no gap among them */
            events_seen++;
        }
        last = seq;
    }
    CHECK(events_seen == TX_QUEUE_ENTRIES);
    CHECK(type == MONITOR_FRAME_SNAPSHOT && last == 34U);
    CHECK(snapshot_uptime(payload, len, &skipped) == view.now_ms && skipped == 4U); /* 30-33 */
}

static void write_frames_file(const char *path)
{
    monitor_view_t view;
    const monitor_periods_t periods = { 10U, 100U, 100U };

    make_view(&view);
    start_link();
    CHECK(monitor_send_snapshot(&view));
    CHECK(monitor_send_config(&periods));
    CHECK(monitor_send_events(&view) == 8U); /* As in test_events(): frames 3-10 */
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fwrite(captured, 1, captured_len, f) == captured_len);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    test_cobs();
    test_snapshot();
    test_config();
    test_latest_wins();
    test_health();
    test_events();
    test_worst_case();
    if (argc > 1) {
        write_frames_file(argv[1]);
    }
    printf("%s (%d failures)\n", (failures == 0) ? "MONITOR PASS" : "MONITOR FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
