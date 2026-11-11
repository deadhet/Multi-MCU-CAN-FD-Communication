/* CAN FD application protocol. Section numbers refer to the Design Document. */
#include "comm/comm.h"
#include "comm/crc16.h"
#include "drivers/fdcan.h"
#include "drivers/systick.h"
#include "drivers/uart.h"

#if NODE_ID < 1 || NODE_ID > 4
#error "NODE_ID must be 1-4"
#endif

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)
#define NODE_TAG "MCU" NUMBER_TEXT(NODE_ID)

/* §5.2: ID = (type << 4) | node. */
#define TYPE_PARAM     2U
#define TYPE_HEARTBEAT 3U
#define MAKE_ID(type, node) (((uint32_t)(type) << 4) | (uint32_t)(node))
#define ID_TYPE(id) ((id) >> 4)
#define ID_NODE(id) ((id) & 0xFU)

/* §5.3: parameter block, 32 bytes. */
#define PARAM_LEN        32U
#define PARAM_STATUS     2U
#define PARAM_VALUES     8U
#define PARAM_CRC        28U
#define PARAM_STATUS_USE 0x03FFU /* Bits 0-9; bit 10 and 11-15 are reserved and sent as 0 */

/* §5.3: heartbeat, 16 bytes. */
#define HB_LEN    16U
#define HB_STATE  2U
#define HB_SYNCED 3U
#define HB_UPTIME 8U
#define HB_ERRORS 12U
#define HB_CRC    14U

/* Shared by both layouts. */
#define BYTE_VERSION  0U
#define BYTE_SOURCE   1U
#define BYTE_SEQUENCE 4U

/* Driver RAM queues: 0 = heartbeats (and our own IDs), 1-3 = the peers' parameter blocks in
 * node order, skipping this node. Read in that order, heartbeats first. */
#define QUEUE_HEARTBEAT 0U
#define PARAM_QUEUE(node) (((node) == NODE_ID) ? QUEUE_HEARTBEAT : (((node) < NODE_ID) ? (node) : ((node) - 1U)))

_Static_assert(FDCAN_RX_QUEUES == COMM_NODE_COUNT, "one heartbeat queue plus one per peer");
_Static_assert(PARAM_VALUES + (COMM_PARAMS_PER_NODE * 4U) == PARAM_CRC, "values must end at the CRC");
_Static_assert(PARAM_CRC + 2U + 2U == PARAM_LEN, "CRC then 2 padding bytes");
_Static_assert(HB_CRC + 2U == HB_LEN, "heartbeat CRC is the last field");

typedef struct {
    uint32_t last_seq;
    uint32_t accepted;
    bool heard;
} stream_t;

static const struct {
    uint32_t type;
    uint8_t len;
    uint8_t crc_offset;
} msg_info[COMM_MSG_COUNT] = {
    [COMM_MSG_PARAM] = { TYPE_PARAM, PARAM_LEN, PARAM_CRC },
    [COMM_MSG_HEARTBEAT] = { TYPE_HEARTBEAT, HB_LEN, HB_CRC },
};

static stream_t streams[COMM_NODE_COUNT][COMM_MSG_COUNT]; /* [sender - 1][message] */
static uint32_t last_uptime[COMM_NODE_COUNT];             /* From the last accepted heartbeat */
static uint32_t restarts[COMM_NODE_COUNT];
static uint32_t tx_seq[COMM_MSG_COUNT];
static comm_tx_stats_t tx_stats;
static comm_errors_t errors;

/* §5.3: every field is written explicitly, little-endian. */
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

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* §5.5: the CAN ID as 2 bytes little-endian (implicit Data ID, not sent), then every payload
 * byte before the CRC. */
static uint16_t frame_crc(uint32_t id, const uint8_t *payload, uint32_t crc_offset)
{
    uint8_t data_id[2];

    put_u16_le(data_id, (uint16_t)id);
    return crc16_update(crc16_update(CRC16_INIT, data_id, 2U), payload, crc_offset);
}

/* Seals and queues one frame. The sequence only moves on if the frame was queued, so it counts
 * transmissions (§5.6). The CRC is the last packing step (§5.5). */
static bool send_frame(comm_msg_t msg, uint8_t *buf)
{
    uint32_t id = MAKE_ID(msg_info[msg].type, NODE_ID);
    uint32_t seq = tx_seq[msg] + 1U;

    buf[BYTE_VERSION] = COMM_PROTOCOL_VERSION;
    buf[BYTE_SOURCE] = (uint8_t)NODE_ID;
    put_u32_le(&buf[BYTE_SEQUENCE], seq);
    put_u16_le(&buf[msg_info[msg].crc_offset], frame_crc(id, buf, msg_info[msg].crc_offset));

    if (!fdcan_send(id, buf, msg_info[msg].len)) {
        tx_stats.not_queued[msg]++;
        return false;
    }
    tx_seq[msg] = seq;
    tx_stats.sent[msg]++;
    return true;
}

bool comm_init(void)
{
    /* Fail fast: a wrong CRC would make every node reject every frame. */
    static const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    if (crc16_update(CRC16_INIT, check, sizeof(check)) != CRC16_CHECK) {
        return false;
    }

    /* Check 0: hardware passes only parameter blocks and heartbeats from nodes 1-4. Our own IDs
     * are included on purpose, so another node using them is seen (duplicate_node). The resync
     * IDs 0x011-0x014 are reserved and rejected.
     * Each range also picks a RAM queue: all heartbeats share one, and so do our own IDs, which
     * only arrive from a duplicate node. Each peer's parameter blocks get their own queue, so a
     * flood from one node cannot push out the others. */
    static const fdcan_id_range_t ranges[] = {
        { MAKE_ID(TYPE_HEARTBEAT, 1U), MAKE_ID(TYPE_HEARTBEAT, COMM_NODE_COUNT), QUEUE_HEARTBEAT },
        { MAKE_ID(TYPE_PARAM, 1U), MAKE_ID(TYPE_PARAM, 1U), PARAM_QUEUE(1U) },
        { MAKE_ID(TYPE_PARAM, 2U), MAKE_ID(TYPE_PARAM, 2U), PARAM_QUEUE(2U) },
        { MAKE_ID(TYPE_PARAM, 3U), MAKE_ID(TYPE_PARAM, 3U), PARAM_QUEUE(3U) },
        { MAKE_ID(TYPE_PARAM, 4U), MAKE_ID(TYPE_PARAM, 4U), PARAM_QUEUE(4U) },
    };
    return fdcan_init(FDCAN_MODE_NORMAL, ranges, sizeof(ranges) / sizeof(ranges[0]));
}

bool comm_send_param_block(const comm_param_block_t *block)
{
    uint8_t buf[PARAM_LEN] = { 0 }; /* Padding bytes 30-31 stay 0 */

    put_u16_le(&buf[PARAM_STATUS], (uint16_t)(block->status & PARAM_STATUS_USE));
    for (uint32_t i = 0U; i < COMM_PARAMS_PER_NODE; i++) {
        put_u32_le(&buf[PARAM_VALUES + (i * 4U)], block->values[i]);
    }
    return send_frame(COMM_MSG_PARAM, buf);
}

bool comm_send_heartbeat(const comm_heartbeat_t *heartbeat)
{
    uint8_t buf[HB_LEN] = { 0 };

    buf[HB_STATE] = heartbeat->state;
    buf[HB_SYNCED] = heartbeat->synced_count;
    put_u32_le(&buf[HB_UPTIME], heartbeat->uptime_ms);
    put_u16_le(&buf[HB_ERRORS], heartbeat->error_summary);
    return send_frame(COMM_MSG_HEARTBEAT, buf);
}

/* §5.7, checks 1-7, cheapest first, stopping at the first failure. Works on the local copy that
 * is then decoded (§5.5). Only an accepted frame changes the stream state. */
static bool validate(const can_rx_slot_t *f, comm_frame_t *out)
{
    /* Check 1: known type, sender 1-4, not this node. */
    uint32_t type = ID_TYPE(f->id);
    uint32_t node = ID_NODE(f->id);
    comm_msg_t msg;

    if (type == TYPE_PARAM) {
        msg = COMM_MSG_PARAM;
    } else if (type == TYPE_HEARTBEAT) {
        msg = COMM_MSG_HEARTBEAT;
    } else {
        errors.bad_id++;
        return false;
    }
    if ((node < 1U) || (node > COMM_NODE_COUNT)) {
        errors.bad_id++;
        return false;
    }
    if (node == NODE_ID) {
        errors.duplicate_node++;
        return false;
    }

    /* Check 2: CAN FD with bitrate switching. */
    if ((f->flags & (CAN_RX_FLAG_FD | CAN_RX_FLAG_BRS)) != (CAN_RX_FLAG_FD | CAN_RX_FLAG_BRS)) {
        errors.bad_format++;
        return false;
    }

    /* Check 3: length before CRC, because the CRC position depends on it. */
    if (f->len != msg_info[msg].len) {
        errors.bad_length++;
        return false;
    }

    /* Check 4. */
    if (f->data[BYTE_VERSION] != COMM_PROTOCOL_VERSION) {
        errors.bad_version++;
        return false;
    }

    /* Check 5. */
    if (f->data[BYTE_SOURCE] != node) {
        errors.bad_source++;
        return false;
    }

    /* Check 6: CRC before sequence, so a corrupted sequence never becomes the last-seen value. */
    uint32_t crc_offset = msg_info[msg].crc_offset;
    if (get_u16_le(&f->data[crc_offset]) != frame_crc(f->id, f->data, crc_offset)) {
        errors.bad_crc++;
        return false;
    }

    /* Check 7 (§5.6): d = seq - last. A NEVER_HEARD stream accepts its first frame. */
    stream_t *s = &streams[node - 1U][msg];
    uint32_t seq = get_u32_le(&f->data[BYTE_SEQUENCE]);
    uint32_t lost = 0U;
    bool restart = false;

    /* §7.3 restart rule: a heartbeat that passed checks 1-6, whose sequence went back, and that
     * says BOOT or shows a lower uptime comes from a node that restarted. Its sequence counters
     * started again at 1, so reset both of its streams and accept this heartbeat as its first
     * frame. The CRC has already been checked, so a corrupted frame cannot trigger this. */
    if (s->heard && (msg == COMM_MSG_HEARTBEAT) && ((int32_t)(seq - s->last_seq) < 0)
        && ((f->data[HB_STATE] == (uint8_t)COMM_STATE_BOOT)
            || (get_u32_le(&f->data[HB_UPTIME]) < last_uptime[node - 1U]))) {
        comm_forget_peer((uint8_t)node);
        restarts[node - 1U]++;
        restart = true;
    }

    if (s->heard) {
        int32_t d = (int32_t)(seq - s->last_seq);
        if (d == 0) {
            errors.duplicate++;
            return false;
        }
        if (d < 0) {
            errors.old_seq++;
            return false;
        }
        lost = (uint32_t)d - 1U;
        errors.lost_frames += lost;
    }
    s->heard = true;
    s->last_seq = seq;
    s->accepted++;

    /* Decode from the same checked copy. */
    out->type = msg;
    out->source = (uint8_t)node;
    out->sequence = seq;
    out->lost_before = lost;
    out->rx_time_ms = f->rx_time_ms;
    out->restart = restart;
    if (msg == COMM_MSG_PARAM) {
        out->body.param.status = get_u16_le(&f->data[PARAM_STATUS]);
        for (uint32_t i = 0U; i < COMM_PARAMS_PER_NODE; i++) {
            out->body.param.values[i] = get_u32_le(&f->data[PARAM_VALUES + (i * 4U)]);
        }
    } else {
        out->body.heartbeat.state = f->data[HB_STATE];
        out->body.heartbeat.synced_count = f->data[HB_SYNCED];
        out->body.heartbeat.uptime_ms = get_u32_le(&f->data[HB_UPTIME]);
        last_uptime[node - 1U] = out->body.heartbeat.uptime_ms;
        out->body.heartbeat.error_summary = get_u16_le(&f->data[HB_ERRORS]);
    }
    return true;
}

bool comm_receive(comm_frame_t *out)
{
    can_rx_slot_t frame; /* Local copy: checked and decoded here, never touched by the ISR */

    /* Heartbeat queue first on every call: it carries health and restart information. Every
     * frame is still validated, so the counters see all of them. */
    for (uint32_t q = 0U; q < FDCAN_RX_QUEUES; q++) {
        while (fdcan_receive(q, &frame)) {
            if (validate(&frame, out)) {
                return true;
            }
        }
    }
    return false;
}

void comm_forget_peer(uint8_t node)
{
    if ((node < 1U) || (node > COMM_NODE_COUNT)) {
        return;
    }
    for (uint32_t m = 0U; m < COMM_MSG_COUNT; m++) {
        streams[node - 1U][m].heard = false;
    }
}

void comm_get_errors(comm_errors_t *out)
{
    fdcan_stats_t drv;

    fdcan_get_stats(&drv);
    *out = errors;
    out->rx_overflow = drv.rx_fifo_lost;
    for (uint32_t q = 0U; q < FDCAN_RX_QUEUES; q++) {
        out->rx_overflow += drv.rx_overwritten[q];
    }
}

uint16_t comm_error_summary(void)
{
    comm_errors_t e;
    uint64_t sum;

    comm_get_errors(&e);
    sum = (uint64_t)e.bad_id + e.duplicate_node + e.bad_format + e.bad_length + e.bad_version
        + e.bad_source + e.bad_crc + e.duplicate + e.old_seq + e.lost_frames + e.value_rejected
        + e.rx_overflow;
    return (sum > 0xFFFFU) ? 0xFFFFU : (uint16_t)sum;
}

bool comm_get_peer_stats(uint8_t node, comm_peer_stats_t *out)
{
    if ((node < 1U) || (node > COMM_NODE_COUNT) || (node == NODE_ID)) {
        return false;
    }
    for (uint32_t m = 0U; m < COMM_MSG_COUNT; m++) {
        const stream_t *s = &streams[node - 1U][m];
        out->accepted[m] = s->accepted;
        out->last_seq[m] = s->last_seq;
        out->heard[m] = s->heard;
    }
    out->restarts = restarts[node - 1U];
    return true;
}

void comm_get_tx_stats(comm_tx_stats_t *out)
{
    *out = tx_stats;
}

static void write_field(const char *name, uint32_t value)
{
    uart_debug_write(name);
    uart_debug_write_u32(value);
}

/* Four lines, for example:
 *   MCU2 t=1005 tx p=100 h=10 nq=0/0
 *   MCU2 rx n1 p=100/100 h=11/11 r=0 n3 p=100/100 h=11/11 r=0 n4 p=100/100 h=11/11 r=0
 *   MCU2 err id=0 dupnode=0 fmt=0 len=0 ver=0 src=0 crc=0 dup=0 old=0 lost=0 val=0 ovf=0
 *   MCU2 drv fifo_lost=0 ovw=0/0/0/0 unrouted=0 tx_full=0
 * In rx, p=a/b means a frames accepted and b the last sequence seen; r counts restarts seen.
 * In drv, ovw lists the frames each RAM queue gave up: heartbeats, then the peers in order. */
void comm_debug_report(void)
{
    comm_errors_t e;
    fdcan_stats_t drv;

    comm_get_errors(&e);
    fdcan_get_stats(&drv);

    write_field(NODE_TAG " t=", systick_now_ms());
    write_field(" tx p=", tx_stats.sent[COMM_MSG_PARAM]);
    write_field(" h=", tx_stats.sent[COMM_MSG_HEARTBEAT]);
    write_field(" nq=", tx_stats.not_queued[COMM_MSG_PARAM]);
    write_field("/", tx_stats.not_queued[COMM_MSG_HEARTBEAT]);
    uart_debug_write("\n" NODE_TAG " rx");
    for (uint32_t n = 1U; n <= COMM_NODE_COUNT; n++) {
        if (n == NODE_ID) {
            continue;
        }
        const stream_t *s = streams[n - 1U];
        write_field(" n", n);
        write_field(" p=", s[COMM_MSG_PARAM].accepted);
        write_field("/", s[COMM_MSG_PARAM].last_seq);
        write_field(" h=", s[COMM_MSG_HEARTBEAT].accepted);
        write_field("/", s[COMM_MSG_HEARTBEAT].last_seq);
        write_field(" r=", restarts[n - 1U]);
    }
    write_field("\n" NODE_TAG " err id=", e.bad_id);
    write_field(" dupnode=", e.duplicate_node);
    write_field(" fmt=", e.bad_format);
    write_field(" len=", e.bad_length);
    write_field(" ver=", e.bad_version);
    write_field(" src=", e.bad_source);
    write_field(" crc=", e.bad_crc);
    write_field(" dup=", e.duplicate);
    write_field(" old=", e.old_seq);
    write_field(" lost=", e.lost_frames);
    write_field(" val=", e.value_rejected);
    write_field(" ovf=", e.rx_overflow);
    write_field("\n" NODE_TAG " drv fifo_lost=", drv.rx_fifo_lost);
    write_field(" ovw=", drv.rx_overwritten[0]);
    for (uint32_t q = 1U; q < FDCAN_RX_QUEUES; q++) {
        write_field("/", drv.rx_overwritten[q]);
    }
    write_field(" unrouted=", drv.rx_unrouted);
    write_field(" tx_full=", drv.tx_fifo_full);
    uart_debug_write("\n");
}
