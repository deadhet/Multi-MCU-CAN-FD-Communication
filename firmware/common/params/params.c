/* Parameter database. Section numbers refer to the Design Document. */
#include <math.h>
#include <stddef.h>
#include "params/params.h"
#include "drivers/uart.h"

#if NODE_ID < 1 || NODE_ID > 4
#error "NODE_ID must be 1-4"
#endif

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)
#define NODE_TAG "MCU" NUMBER_TEXT(NODE_ID)

/* §6.2: a received parameter is STALE when its last block is older than 3 x the 10 ms period,
 * so two lost frames in a row are tolerated. */
#define TRANSPORT_STALE_MS 30U

/* Settle rule (Design Document open item 2, Het, 11 Nov 2026). After a sender was silent for
 * more than TRANSPORT_STALE_MS, its parameters stay STALE until one of its blocks arrives
 * SETTLE_MIN_GAP_MS to TRANSPORT_STALE_MS after the previous one, the normal 10 ms beat.
 * Frames left in the sender's Tx FIFO by a disconnect go out back-to-back at reconnect with
 * values from before it; they arrive under 1 ms apart, so they are stored but never count. */
#define SETTLE_MIN_GAP_MS 5U

#define STATUS_BITS 2U
#define STATUS_MASK 0x3U
#define FIRST_ID(node) ((((node) - 1U) * PARAMS_PER_NODE) + 1U)

_Static_assert(PARAMS_PER_NODE == COMM_PARAMS_PER_NODE, "a block carries one node's parameters");
_Static_assert(PARAM_COUNT == COMM_NODE_COUNT * PARAMS_PER_NODE, "20 parameters, 5 per node");

/* Live part (§6.1). Age is never stored; it is computed from last_update_ms when asked. */
typedef struct {
    param_value_t value;
    uint32_t seq;
    uint32_t last_update_ms;
    param_validity_t validity;
    param_freshness_t freshness;
    uint8_t source_status;
    param_validity_t logged_validity; /* What the log last showed, to print changes only */
    param_freshness_t logged_freshness;
} entry_t;

static entry_t db[PARAM_COUNT];
static params_stats_t stats;
static uint32_t last_block_ms[COMM_NODE_COUNT];
static bool block_seen[COMM_NODE_COUNT];
static bool settling[COMM_NODE_COUNT]; /* Silent sender back, no block on the beat yet */

/* Check 8 on one value. The range check uses the member that matches the type. NaN fails
 * every comparison and is caught by isfinite() anyway. */
static bool value_ok(const param_def_t *def, param_value_t v)
{
    switch (def->type) {
    case PARAM_TYPE_FLOAT32:
        return isfinite(v.f) && (v.f >= def->min.f) && (v.f <= def->max.f);
    case PARAM_TYPE_INT32:
        return (v.i >= def->min.i) && (v.i <= def->max.i);
    case PARAM_TYPE_UINT32:
    case PARAM_TYPE_BOOL:
        return (v.u >= def->min.u) && (v.u <= def->max.u);
    default:
        return false;
    }
}

void params_init(void)
{
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        db[i] = (entry_t){
            .validity = PARAM_NEVER_RECEIVED,
            .freshness = PARAM_STALE,
            .logged_validity = PARAM_NEVER_RECEIVED,
            .logged_freshness = PARAM_STALE,
            /* Own parameters say SOURCE_STALE until their first sample */
            .source_status = (uint8_t)PARAM_SOURCE_STALE,
        };
    }
    stats = (params_stats_t){ 0 };
    for (uint32_t n = 0U; n < COMM_NODE_COUNT; n++) {
        last_block_ms[n] = 0U;
        block_seen[n] = false;
        settling[n] = false;
    }
}

bool params_set_local(uint8_t id, param_value_t value, uint32_t now_ms)
{
    const param_def_t *def = system_table_get(id);

    if ((def == NULL) || (def->owner != NODE_ID)) {
        return false;
    }
    entry_t *e = &db[id - 1U];
    bool ok = value_ok(def, value);

    if (ok) {
        e->value = value;
        e->validity = PARAM_VALID;
        e->source_status = (uint8_t)PARAM_SOURCE_OK;
    } else {
        e->validity = PARAM_INVALID; /* Old value kept, flagged */
        e->source_status = (uint8_t)PARAM_SOURCE_FAULT;
    }
    /* A sample was taken either way, so the timestamp moves on (Het, 11 Nov 2026). */
    e->seq++;
    e->last_update_ms = now_ms;
    return ok;
}

void params_build_block(comm_param_block_t *out)
{
    out->status = 0U;
    for (uint32_t slot = 0U; slot < PARAMS_PER_NODE; slot++) {
        const entry_t *e = &db[FIRST_ID(NODE_ID) - 1U + slot];

        out->values[slot] = e->value.u; /* The raw 4 bytes, whatever the type */
        out->status |= (uint16_t)((uint16_t)e->source_status << (slot * STATUS_BITS));
    }
}

void params_apply_block(uint8_t node, const comm_param_block_t *block, uint32_t seq, uint32_t rx_time_ms)
{
    if ((node < 1U) || (node > COMM_NODE_COUNT) || (node == NODE_ID)) {
        return; /* Our own parameters are written only by params_set_local() */
    }

    /* §6.2: arrival gap per sender, evidence that the 10 ms period holds. */
    uint32_t n = node - 1U;
    if (block_seen[n]) {
        uint32_t gap = rx_time_ms - last_block_ms[n];
        stats.last_gap_ms[n] = gap;
        if (gap > stats.worst_gap_ms[n]) {
            stats.worst_gap_ms[n] = gap;
        }
        /* Settle rule. The first block ever needs none: a sender's Tx FIFO is empty at boot. */
        if (gap > TRANSPORT_STALE_MS) {
            settling[n] = true; /* Back after a silence: this block may be an old one */
        } else if (gap >= SETTLE_MIN_GAP_MS) {
            settling[n] = false; /* On the beat: sampled after the silence */
        } else {
            /* Too close to the previous block: settling stays as it is */
        }
    }
    if (settling[n]) {
        stats.held[n]++;
    }
    block_seen[n] = true;
    last_block_ms[n] = rx_time_ms;
    stats.blocks[n]++;

    for (uint32_t slot = 0U; slot < PARAMS_PER_NODE; slot++) {
        uint8_t id = (uint8_t)(FIRST_ID(node) + slot);
        const param_def_t *def = system_table_get(id);
        entry_t *e = &db[id - 1U];
        uint8_t status = (uint8_t)((block->status >> (slot * STATUS_BITS)) & STATUS_MASK);
        param_value_t v = { .u = block->values[slot] };

        /* Check 8. SOURCE_FAULT is the owner's own report, not a transport error, so it makes
         * the value INVALID without counting. The reserved code and a bad value are counted. */
        if (status == (uint8_t)PARAM_SOURCE_FAULT) {
            e->validity = PARAM_INVALID;
        } else if ((status > (uint8_t)PARAM_SOURCE_FAULT) || !value_ok(def, v)) {
            e->validity = PARAM_INVALID;
            stats.value_rejected++;
        } else {
            e->value = v;
            e->validity = PARAM_VALID;
        }
        /* An INVALID value keeps the old content, but the timestamp and sequence still move on,
         * because the frame did arrive (Het, 11 Nov 2026). */
        e->source_status = status;
        e->seq = seq;
        e->last_update_ms = rx_time_ms;
    }
}

/* Prints "MCUn t=<now> db <label> id id ..." for every parameter whose logged state differs
 * and now equals the wanted one, then records it as logged. Prints nothing if none changed. */
static void log_changes(uint32_t now_ms, const char *label, bool freshness, uint32_t wanted)
{
    bool header = false;

    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        entry_t *e = &db[i];
        uint32_t current = freshness ? (uint32_t)e->freshness : (uint32_t)e->validity;
        uint32_t logged = freshness ? (uint32_t)e->logged_freshness : (uint32_t)e->logged_validity;

        if ((current == logged) || (current != wanted)) {
            continue;
        }
        if (!header) {
            uart_debug_write(NODE_TAG " t=");
            uart_debug_write_u32(now_ms);
            uart_debug_write(" db ");
            uart_debug_write(label);
            header = true;
        }
        uart_debug_write(" ");
        uart_debug_write_u32(i + 1U);
        if (freshness) {
            e->logged_freshness = e->freshness;
        } else {
            e->logged_validity = e->validity;
        }
    }
    if (header) {
        uart_debug_write("\n");
    }
}

void params_update_freshness(uint32_t now_ms)
{
    for (uint8_t id = 1U; id <= PARAM_COUNT; id++) {
        const param_def_t *def = system_table_get(id);
        entry_t *e = &db[id - 1U];
        uint32_t age = now_ms - e->last_update_ms;
        bool stale;

        if (e->validity == PARAM_NEVER_RECEIVED) {
            stale = true;
        } else if (def->owner == NODE_ID) {
            /* §6.3: our own sample is stale after 3 x its sample period, and we say so. */
            stale = age > def->source_stale_ms;
            if (e->validity == PARAM_VALID) {
                e->source_status = (uint8_t)(stale ? PARAM_SOURCE_STALE : PARAM_SOURCE_OK);
            }
        } else {
            /* Transport stale, the owner says its sample is stale, or the owner is back after
             * a silence and no block on the beat has arrived yet (settle rule). */
            stale = (age > TRANSPORT_STALE_MS) || (e->source_status == (uint8_t)PARAM_SOURCE_STALE)
                    || settling[def->owner - 1U];
        }
        e->freshness = stale ? PARAM_STALE : PARAM_FRESH;
    }

    log_changes(now_ms, "VALID", false, (uint32_t)PARAM_VALID);
    log_changes(now_ms, "INVALID", false, (uint32_t)PARAM_INVALID);
    log_changes(now_ms, "FRESH", true, (uint32_t)PARAM_FRESH);
    log_changes(now_ms, "STALE", true, (uint32_t)PARAM_STALE);
}

bool params_get(uint8_t id, uint32_t now_ms, param_entry_t *out)
{
    const param_def_t *def = system_table_get(id);

    if (def == NULL) {
        return false;
    }
    const entry_t *e = &db[id - 1U];
    *out = (param_entry_t){
        .id = id,
        .owner = def->owner,
        .type = def->type,
        .value = e->value,
        .seq = e->seq,
        .last_update_ms = e->last_update_ms,
        .age_ms = now_ms - e->last_update_ms,
        .validity = e->validity,
        .freshness = e->freshness,
        .source_status = e->source_status,
    };
    return true;
}

uint8_t params_synced_count(void)
{
    uint8_t count = 0U;

    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        if ((db[i].validity == PARAM_VALID) && (db[i].freshness == PARAM_FRESH)) {
            count++;
        }
    }
    return count;
}

uint32_t params_value_rejected(void)
{
    return stats.value_rejected;
}

void params_get_stats(params_stats_t *out)
{
    *out = stats;
}

static void write_field(const char *name, uint32_t value)
{
    uart_debug_write(name);
    uart_debug_write_u32(value);
}

/* Two lines, for example:
 *   MCU2 db synced=20 valid=20 invalid=0 stale=0 never=0 rej=0 gap n1=10/11 n3=10/11 n4=10/11 held n1=0 n3=0 n4=0
 *   MCU2 val 1=0 2=1132068864 ... 20=1
 * gap nX=a/b is the last and the worst arrival gap of node X's blocks in ms; held nX counts
 * node X's blocks that arrived while it was settling after a silence. Values are the raw
 * 4 bytes as uint32 (a float shows its bit pattern), so logs of different nodes can be compared
 * exactly. */
void params_debug_report(void)
{
    uint32_t valid = 0U;
    uint32_t invalid = 0U;
    uint32_t stale = 0U;
    uint32_t never = 0U;

    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        valid += (db[i].validity == PARAM_VALID) ? 1U : 0U;
        invalid += (db[i].validity == PARAM_INVALID) ? 1U : 0U;
        never += (db[i].validity == PARAM_NEVER_RECEIVED) ? 1U : 0U;
        stale += (db[i].freshness == PARAM_STALE) ? 1U : 0U;
    }
    write_field(NODE_TAG " db synced=", params_synced_count());
    write_field(" valid=", valid);
    write_field(" invalid=", invalid);
    write_field(" stale=", stale);
    write_field(" never=", never);
    write_field(" rej=", stats.value_rejected);
    uart_debug_write(" gap");
    for (uint32_t n = 1U; n <= COMM_NODE_COUNT; n++) {
        if (n == NODE_ID) {
            continue;
        }
        write_field(" n", n);
        write_field("=", stats.last_gap_ms[n - 1U]);
        write_field("/", stats.worst_gap_ms[n - 1U]);
    }
    uart_debug_write(" held");
    for (uint32_t n = 1U; n <= COMM_NODE_COUNT; n++) {
        if (n == NODE_ID) {
            continue;
        }
        write_field(" n", n);
        write_field("=", stats.held[n - 1U]);
    }
    uart_debug_write("\n" NODE_TAG " val");
    for (uint32_t i = 0U; i < PARAM_COUNT; i++) {
        write_field(" ", i + 1U);
        write_field("=", db[i].value.u);
    }
    uart_debug_write("\n");
}
