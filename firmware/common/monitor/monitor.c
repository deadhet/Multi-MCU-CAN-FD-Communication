/* UART link protocol. Section numbers refer to the Design Document. Linked into node1 only. */
#include "monitor/monitor.h"
#include "monitor/cobs.h"
#include "monitor/monitor.pb.h"
#include "drivers/fdcan.h"
#include "drivers/uart_link.h"
#include "system_table.h"
#include "util/crc16.h"
#include "util/noinit.h"
#include "pb_encode.h"

#if NODE_ID != 1
#error "monitor.c is for MCU1 only"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0-dev"
#endif

/* Frame: [version << 4 | type][sequence, uint32 LE][payload][CRC-16 LE]. */
#define FRAME_HEADER 5U
#define FRAME_CRC    2U
#define PAYLOAD_MAX  MONITOR_MONITOR_PB_H_MAX_SIZE /* Largest message: from the generator */
#define FRAME_MAX    (FRAME_HEADER + PAYLOAD_MAX + FRAME_CRC)

/* On the wire: COBS, plus the 0x00 terminator. One size per kind, from the generator. */
#define WIRE_MAX(payload_max) (COBS_MAX_ENCODED(FRAME_HEADER + (payload_max) + FRAME_CRC) + 1U)
#define SNAPSHOT_WIRE_MAX     WIRE_MAX(monitor_Snapshot_size)
#define CONFIG_WIRE_MAX       WIRE_MAX(monitor_Config_size)
#define EVENT_WIRE_MAX        WIRE_MAX(monitor_Event_size)

#define LINK_STATES 8U /* monitor_LinkState values 0-7 (UNKNOWN-SELF) */

/* The schema's enums are numbered like ours, so values are copied without a table. */
_Static_assert((int)monitor_Validity_NEVER_RECEIVED == (int)PARAM_NEVER_RECEIVED, "validity");
_Static_assert((int)monitor_Validity_VALID == (int)PARAM_VALID, "validity");
_Static_assert((int)monitor_Validity_INVALID == (int)PARAM_INVALID, "validity");
_Static_assert((int)monitor_Freshness_STALE == (int)PARAM_STALE, "freshness");
_Static_assert((int)monitor_Freshness_FRESH == (int)PARAM_FRESH, "freshness");
_Static_assert((int)monitor_SourceStatus_SOURCE_FAULT == (int)PARAM_SOURCE_FAULT, "source status");
_Static_assert((int)monitor_NodeState_BOOT == (int)COMM_STATE_BOOT, "node state");
_Static_assert((int)monitor_NodeState_OPERATIONAL == (int)COMM_STATE_OPERATIONAL, "node state");
_Static_assert((int)monitor_LinkState_UNKNOWN == (int)DIAG_UNKNOWN, "link state");
_Static_assert((int)monitor_LinkState_ONLINE == (int)DIAG_ONLINE, "link state");
_Static_assert((int)monitor_LinkState_OFFLINE == (int)DIAG_OFFLINE, "link state");
_Static_assert((int)monitor_LinkState_DATA_INVALID == (int)DIAG_DATA_INVALID, "link state");
_Static_assert(PAYLOAD_MAX < 65536U, "frame lengths must stay small");
_Static_assert((int)monitor_LinkState_SELF == (int)(LINK_STATES - 1U), "link states");
_Static_assert(PARAM_COUNT <= 32U && COMM_NODE_COUNT <= 32U, "event masks are 32 bits");

/* The UART buffers, in .noinit RAM (Het, 12 Nov 2026): start-up does not clear them, and a DMA
 * can reach them. Each is completely written before it is read. Static, not on the stack: the
 * structs are close to 1 KB each.
 *   snapshot, config     the message, filled field by field, then encoded into frame
 *   frame                header + Protobuf payload + CRC, before COBS
 *   *_wire               the two slot buffers per kind (drivers/tx_slots.h): COBS + 0x00
 *   event_wire           the event queue's entries */
static monitor_Snapshot snapshot NOINIT_RAM;
static monitor_Config config NOINIT_RAM;
static uint8_t frame[FRAME_MAX] NOINIT_RAM;
static uint8_t snapshot_wire[2][SNAPSHOT_WIRE_MAX] NOINIT_RAM;
static uint8_t config_wire[2][CONFIG_WIRE_MAX] NOINIT_RAM;
static uint8_t event_wire[TX_QUEUE_ENTRIES][EVENT_WIRE_MAX] NOINIT_RAM;

static uint32_t uart_seq; /* Last sequence used; the first frame is 1 */
static monitor_stats_t stats;

/* What the previous monitor_send_events() saw; a difference is an event. */
static struct {
    uint8_t validity[PARAM_COUNT];
    uint8_t freshness[PARAM_COUNT];
    bool owner_offline[PARAM_COUNT];
    uint8_t link[COMM_NODE_COUNT];
    uint32_t restarts[COMM_NODE_COUNT];
    uint8_t own_state;
} seen;

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Step 1 of sending: the slot buffer for the next frame of this kind. A frame of this kind still
 * waiting is taken back here, before the message is filled, so the snapshot built next already
 * counts it in frames_skipped. */
static uint8_t *begin_frame(tx_slot_t slot, uint32_t *size)
{
    bool replaced = false;
    uint8_t *out = uart_link_begin(slot, size, &replaced);

    if (replaced) {
        stats.skipped++;
    }
    return out;
}

/* Step 2: encodes msg and frames it into out (size bytes), with the next sequence number.
 * Returns the bytes on the wire, or 0 if it could not be encoded. The caller hands the frame
 * over and then advances uart_seq. The sequence advances for every frame handed over, and for
 * every event the full queue refused: a gap at the host is a frame counted in frames_skipped
 * (replaced, or refused) or bytes lost on the way. */
static uint32_t build_frame(uint8_t *out, uint32_t size, uint8_t type, const pb_msgdesc_t *fields,
                            const void *msg)
{
    pb_ostream_t stream = pb_ostream_from_buffer(&frame[FRAME_HEADER], PAYLOAD_MAX);

    if ((out == NULL) || !pb_encode(&stream, fields, msg)) {
        stats.encode_errors++;
        return 0U;
    }
    uint32_t len = FRAME_HEADER + (uint32_t)stream.bytes_written;
    frame[0] = (uint8_t)((MONITOR_PROTOCOL_VERSION << 4) | type);
    put_u32_le(&frame[1], uart_seq + 1U);
    put_u16_le(&frame[len], crc16_update(CRC16_INIT, frame, len));
    len += FRAME_CRC;

    if ((COBS_MAX_ENCODED(len) + 1U) > size) {
        stats.encode_errors++; /* Cannot happen: the slot buffers are sized from the generator */
        return 0U;
    }
    uint32_t n = cobs_encode(frame, len, out);
    out[n++] = 0x00U;
    return n;
}

/* Builds a SNAPSHOT or CONFIG frame into its slot buffer and hands it over. */
static uint32_t send_frame(tx_slot_t slot, uint8_t *out, uint32_t size, uint8_t type,
                           const pb_msgdesc_t *fields, const void *msg)
{
    uint32_t n = build_frame(out, size, type, fields, msg);

    if (n != 0U) {
        uart_link_commit(slot, n);
        uart_seq++;
    }
    return n;
}

/* Builds one EVENT frame into the queue and hands it over. If the queue is full the event is
 * lost: it still uses up its sequence number and counts in frames_skipped, so the host sees the
 * gap explained. The next snapshot shows the state anyway. */
static void send_event(const monitor_Event *e)
{
    uint32_t size = 0U;
    uint8_t *out = uart_link_queue_begin(&size);

    if (out == NULL) {
        stats.events_dropped++;
        stats.skipped++;
        uart_seq++;
        return;
    }
    uint32_t n = build_frame(out, size, MONITOR_FRAME_EVENT, monitor_Event_fields, e);
    if (n != 0U) {
        uart_link_queue_commit(n);
        uart_seq++;
        stats.events++;
    }
}

void monitor_init(void)
{
    uart_seq = 0U;
    stats = (monitor_stats_t){ 0 };
    uart_link_attach(TX_SLOT_SNAPSHOT, snapshot_wire[0], snapshot_wire[1], SNAPSHOT_WIRE_MAX);
    uart_link_attach(TX_SLOT_CONFIG, config_wire[0], config_wire[1], CONFIG_WIRE_MAX);
    uart_link_attach_queue(&event_wire[0][0], EVENT_WIRE_MAX);

    /* Events report changes from the boot state: nothing received, every peer unknown. */
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        seen.validity[i] = (uint8_t)PARAM_NEVER_RECEIVED;
        seen.freshness[i] = (uint8_t)PARAM_STALE;
        seen.owner_offline[i] = false;
    }
    for (uint32_t n = 0U; n < COMM_NODE_COUNT; n++) {
        seen.link[n] = (uint8_t)DIAG_UNKNOWN;
        seen.restarts[n] = 0U;
    }
    seen.own_state = (uint8_t)COMM_STATE_BOOT;
}

static void fill_parameter(monitor_Parameter *p, const param_entry_t *e)
{
    *p = (monitor_Parameter)monitor_Parameter_init_zero;
    p->id = e->id;
    p->source = e->owner;
    switch (e->type) {
    case PARAM_TYPE_FLOAT32:
        p->which_value = monitor_Parameter_f32_tag;
        p->value.f32 = e->value.f;
        break;
    case PARAM_TYPE_INT32:
        p->which_value = monitor_Parameter_i32_tag;
        p->value.i32 = e->value.i;
        break;
    case PARAM_TYPE_UINT32:
        p->which_value = monitor_Parameter_u32_tag;
        p->value.u32 = e->value.u;
        break;
    case PARAM_TYPE_BOOL:
    default:
        p->which_value = monitor_Parameter_flag_tag;
        p->value.flag = (e->value.u != 0U);
        break;
    }
    p->validity = (monitor_Validity)e->validity;
    p->freshness = (monitor_Freshness)e->freshness;
    p->age_ms = e->age_ms;
    p->sequence = e->seq;
    p->source_status = (monitor_SourceStatus)e->source_status;
    p->owner_offline = e->owner_offline;
}

static void fill_node(monitor_NodeHealth *h, uint8_t node, const monitor_view_t *view)
{
    *h = (monitor_NodeHealth)monitor_NodeHealth_init_zero;
    h->node = node;
    if (node == NODE_ID) {
        h->link = monitor_LinkState_SELF;
        h->reported_state = (monitor_NodeState)view->own_state;
        h->synced_count = view->synced_count;
        h->error_summary = view->own_error_summary;
        return;
    }
    const diag_peer_t *p = &view->peers[node - 1U];
    h->link = (monitor_LinkState)p->link;
    h->reported_state = p->heard ? (monitor_NodeState)p->last.state : monitor_NodeState_BOOT;
    h->heartbeat_age_ms = p->heartbeat_age_ms;
    h->missed = p->missed;
    h->restarts = p->restarts;
    h->recoveries = p->recoveries;
    h->synced_count = p->last.synced_count;
    h->error_summary = p->last.error_summary;
    h->data_age_ms = p->data_age_ms;
}

uint32_t monitor_online_count(const monitor_view_t *view)
{
    uint32_t online = 1U; /* This node */

    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        if ((node != NODE_ID) && (view->peers[node - 1U].link == DIAG_ONLINE)) {
            online++;
        }
    }
    return online;
}

uint32_t monitor_health_percent(uint32_t online, uint32_t synced)
{
    /* Each part in whole percent, rounded down; then the weaker one. Rounding each part down
     * and then taking the smaller gives the same as rounding the smaller part down. */
    uint32_t nodes_part = (online * 100U) / COMM_NODE_COUNT;
    uint32_t params_part = (synced * 100U) / PARAM_COUNT;
    uint32_t health = (nodes_part < params_part) ? nodes_part : params_part;

    return (health > 100U) ? 100U : health;
}

bool monitor_send_snapshot(const monitor_view_t *view)
{
    uint32_t size = 0U;
    uint8_t *out = begin_frame(TX_SLOT_SNAPSHOT, &size);

    snapshot = (monitor_Snapshot)monitor_Snapshot_init_zero;
    snapshot.uptime_ms = view->now_ms;
    snapshot.nodes_count = COMM_NODE_COUNT;
    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        fill_node(&snapshot.nodes[node - 1U], node, view);
    }
    snapshot.params_count = PARAM_COUNT;
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        fill_parameter(&snapshot.params[i], &view->params[i]);
    }
    snapshot.online_count = monitor_online_count(view);
    snapshot.synced_count = view->synced_count;
    snapshot.frames_skipped = stats.skipped;
    snapshot.health_percent = monitor_health_percent(snapshot.online_count, snapshot.synced_count);

    uint32_t n = send_frame(TX_SLOT_SNAPSHOT, out, size, MONITOR_FRAME_SNAPSHOT, monitor_Snapshot_fields, &snapshot);
    if (n == 0U) {
        return false;
    }
    stats.snapshots++;
    stats.last_snapshot_bytes = n;
    if (n > stats.max_snapshot_bytes) {
        stats.max_snapshot_bytes = n;
    }
    return true;
}

static void copy_text(char *dst, uint32_t size, const char *src)
{
    uint32_t i = 0U;

    while ((i + 1U < size) && (src[i] != '\0')) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

bool monitor_send_config(const monitor_periods_t *periods)
{
    uint32_t size = 0U;
    uint8_t *out = begin_frame(TX_SLOT_CONFIG, &size);

    config = (monitor_Config)monitor_Config_init_zero;
    config.protocol_version = MONITOR_PROTOCOL_VERSION;
    copy_text(config.firmware, sizeof(config.firmware), FIRMWARE_VERSION);
    config.arbitration_bps = fdcan_nominal_bitrate();
    config.data_bps = fdcan_data_bitrate();
    config.brs = fdcan_brs_enabled();
    config.node_count = COMM_NODE_COUNT;
    config.param_count = PARAM_COUNT;
    config.param_period_ms = periods->param_period_ms;
    config.heartbeat_period_ms = periods->heartbeat_period_ms;
    config.snapshot_period_ms = periods->snapshot_period_ms;
    config.params_count = PARAM_COUNT;
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        const param_def_t *def = system_table_get(id);
        monitor_Config_ParamInfo *info = &config.params[id - 1U];
        info->id = id;
        copy_text(info->name, sizeof(info->name), def->name);
        info->owner = def->owner;
        info->sample_period_ms = def->sample_period_ms;
    }

    if (send_frame(TX_SLOT_CONFIG, out, size, MONITOR_FRAME_CONFIG, monitor_Config_fields, &config) == 0U) {
        return false;
    }
    stats.configs++;
    return true;
}

static void send_node_event(uint32_t now_ms, uint32_t nodes, bool restarted, uint8_t link)
{
    monitor_Event e = monitor_Event_init_zero;

    e.time_ms = now_ms;
    e.which_body = monitor_Event_node_tag;
    e.body.node.nodes = nodes;
    if (restarted) {
        e.body.node.which_change = monitor_NodeEvent_restarted_tag;
        e.body.node.change.restarted = true;
    } else {
        e.body.node.which_change = monitor_NodeEvent_link_tag;
        e.body.node.change.link = (monitor_LinkState)link;
    }
    send_event(&e);
}

uint32_t monitor_send_events(const monitor_view_t *view)
{
    uint32_t before = stats.events + stats.events_dropped;
    monitor_Event e = monitor_Event_init_zero;

    /* Nodes first, as a cause comes before its effects on the parameters. */
    uint32_t restarted = 0U;
    uint32_t moved_to[LINK_STATES] = { 0U };
    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        const diag_peer_t *p = &view->peers[node - 1U];
        uint32_t bit = 1UL << (node - 1U);
        if ((node == NODE_ID) || ((uint32_t)p->link >= LINK_STATES)) {
            continue;
        }
        if (p->restarts != seen.restarts[node - 1U]) {
            restarted |= bit;
            seen.restarts[node - 1U] = p->restarts;
        }
        if ((uint8_t)p->link != seen.link[node - 1U]) {
            moved_to[p->link] |= bit;
            seen.link[node - 1U] = (uint8_t)p->link;
        }
    }
    if (restarted != 0U) {
        send_node_event(view->now_ms, restarted, true, 0U);
    }
    for (uint8_t link = 0U; link < LINK_STATES; link++) {
        if (moved_to[link] != 0U) {
            send_node_event(view->now_ms, moved_to[link], false, link);
        }
    }
    if ((uint8_t)view->own_state != seen.own_state) {
        e.time_ms = view->now_ms;
        e.which_body = monitor_Event_own_state_tag;
        e.body.own_state = (monitor_NodeState)view->own_state;
        send_event(&e);
        seen.own_state = (uint8_t)view->own_state;
    }

    /* Parameters, one event per kind of change, each naming every parameter that made it. */
    uint32_t changed[monitor_ParamChange_OWNER_BACK + 1] = { 0U };
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        const param_entry_t *pe = &view->params[i];
        uint32_t bit = 1UL << i;
        if (pe->owner_offline != seen.owner_offline[i]) {
            changed[pe->owner_offline ? monitor_ParamChange_OWNER_WENT_OFFLINE : monitor_ParamChange_OWNER_BACK] |= bit;
            seen.owner_offline[i] = pe->owner_offline;
        }
        if ((uint8_t)pe->validity != seen.validity[i]) {
            /* Not VALID any more is INVALID, for the host (NEVER_RECEIVED cannot come back). */
            changed[(pe->validity == PARAM_VALID) ? monitor_ParamChange_BECAME_VALID : monitor_ParamChange_BECAME_INVALID] |= bit;
            seen.validity[i] = (uint8_t)pe->validity;
        }
        if ((uint8_t)pe->freshness != seen.freshness[i]) {
            changed[(pe->freshness == PARAM_FRESH) ? monitor_ParamChange_BECAME_FRESH : monitor_ParamChange_BECAME_STALE] |= bit;
            seen.freshness[i] = (uint8_t)pe->freshness;
        }
    }
    static const monitor_ParamChange order[] = {
        monitor_ParamChange_OWNER_WENT_OFFLINE, monitor_ParamChange_OWNER_BACK,
        monitor_ParamChange_BECAME_VALID,       monitor_ParamChange_BECAME_INVALID,
        monitor_ParamChange_BECAME_FRESH,       monitor_ParamChange_BECAME_STALE,
    };
    for (uint32_t k = 0U; k < sizeof(order) / sizeof(order[0]); k++) {
        if (changed[order[k]] != 0U) {
            e = (monitor_Event)monitor_Event_init_zero;
            e.time_ms = view->now_ms;
            e.which_body = monitor_Event_param_tag;
            e.body.param.change = order[k];
            e.body.param.params = changed[order[k]];
            send_event(&e);
        }
    }
    return (stats.events + stats.events_dropped) - before;
}

void monitor_get_stats(monitor_stats_t *out)
{
    *out = stats;
}
