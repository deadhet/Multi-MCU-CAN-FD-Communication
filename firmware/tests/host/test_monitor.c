/* PC test for the UART link: monitor/monitor.c, monitor/cobs.c, util/crc16.c and nanopb, built as
 * node 1. The UART is replaced by a capture buffer. Frames are decoded back here with nanopb,
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

/* ---- Stand-ins for the drivers ---- */
static uint8_t captured[16384];
static uint32_t captured_len;
static uint32_t link_free = 4095U;

uint32_t uart_link_free(void) { return link_free; }

bool uart_link_send(const uint8_t *data, uint32_t len)
{
    if ((len > link_free) || (captured_len + len > sizeof(captured))) {
        return false;
    }
    memcpy(&captured[captured_len], data, len);
    captured_len += len;
    return true;
}

void uart_link_get_stats(uart_link_stats_t *out) { memset(out, 0, sizeof(*out)); }
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
    monitor_init();
    captured_len = 0U;
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
    CHECK(s.online_count == 2U && s.synced_count == 12U && s.snapshots_skipped == 0U);

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

static void test_overflow_rule(void)
{
    printf("Overflow rule (§8.4): snapshot needs its size + 256 free, CONFIG may use the reserve\n");
    monitor_view_t view;
    make_view(&view);
    monitor_init();
    captured_len = 0U;
    CHECK(monitor_send_snapshot(&view));
    uint32_t snap_bytes = captured_len;

    link_free = snap_bytes + MONITOR_RESERVE_BYTES - 1U;
    CHECK(!monitor_send_snapshot(&view));
    monitor_stats_t st;
    monitor_get_stats(&st);
    CHECK(st.skipped == 1U && st.snapshots == 1U && captured_len == snap_bytes);
    /* The next snapshot reports snapshots_skipped = 1: 2 bytes more (tag and value). */
    link_free = snap_bytes + 2U + MONITOR_RESERVE_BYTES - 1U;
    CHECK(!monitor_send_snapshot(&view));
    link_free = snap_bytes + 2U + MONITOR_RESERVE_BYTES;
    CHECK(monitor_send_snapshot(&view));

    /* The sequence counts queued frames only: no gap after a skipped snapshot. */
    uint32_t pos = 0U, seq = 0U, len = 0U;
    uint8_t type = 0U, payload[2048];
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && seq == 1U);
    CHECK(take_frame(&pos, &type, &seq, payload, &len) && seq == 2U);

    const monitor_periods_t periods = { 10U, 100U, 100U };
    link_free = 100U;
    CHECK(!monitor_send_config(&periods));
    monitor_get_stats(&st);
    CHECK(st.refused == 1U && st.skipped == 2U);
    link_free = 4095U;
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
    monitor_init();
    captured_len = 0U;
    CHECK(monitor_send_snapshot(&view));
    uint32_t pos = 0U, seq = 0U, len = 0U;
    uint8_t type = 0U, payload[2048];
    CHECK(take_frame(&pos, &type, &seq, payload, &len));
    CHECK(len <= monitor_Snapshot_size);
    printf("  worst snapshot: %u payload bytes, %u on the wire\n", (unsigned)len, (unsigned)captured_len);
}

static void write_frames_file(const char *path)
{
    monitor_view_t view;
    const monitor_periods_t periods = { 10U, 100U, 100U };

    make_view(&view);
    monitor_init();
    captured_len = 0U;
    CHECK(monitor_send_snapshot(&view));
    CHECK(monitor_send_config(&periods));
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
    test_overflow_rule();
    test_worst_case();
    if (argc > 1) {
        write_frames_file(argv[1]);
    }
    printf("%s (%d failures)\n", (failures == 0) ? "MONITOR PASS" : "MONITOR FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
