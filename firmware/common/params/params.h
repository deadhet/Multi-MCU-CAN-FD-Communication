/* Parameter database: all 20 parameters, this node's own and the received ones (Design
 * Document §6). The definitions come from system_table.c; this module holds the live part.
 * Called from the main loop only, so it needs no locking (§4.3). Other modules read copies,
 * never pointers. Times are passed in, so the module also runs on a PC. */
#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stdint.h>
#include "comm/comm.h"
#include "system_table.h"

/* 2-bit source status per parameter in the block's status word (§5.3). */
typedef enum {
    PARAM_SOURCE_OK = 0,
    PARAM_SOURCE_STALE = 1, /* The owner's own sample is too old */
    PARAM_SOURCE_FAULT = 2  /* The owner's sample failed; 3 is reserved and treated as invalid */
} param_source_status_t;

/* §6.2: two independent questions. Can the content be trusted, and is it recent? */
typedef enum {
    PARAM_NEVER_RECEIVED = 0,
    PARAM_VALID = 1,
    PARAM_INVALID = 2 /* The old value is kept, flagged */
} param_validity_t;

typedef enum {
    PARAM_STALE = 0,
    PARAM_FRESH = 1
} param_freshness_t;

/* One parameter as handed out by params_get(). */
typedef struct {
    uint8_t id;    /* 1-20 */
    uint8_t owner; /* Source MCU, 1-4 */
    param_type_t type;
    param_value_t value;
    uint32_t seq;            /* Received: sequence of the block that last updated it. Own: samples taken */
    uint32_t last_update_ms; /* Received: arrival time. Own: sample time */
    uint32_t age_ms;         /* now - last_update_ms at the call; never stored */
    param_validity_t validity;
    param_freshness_t freshness;
    uint8_t source_status; /* param_source_status_t as last sent or received */
    bool owner_offline;    /* Its owner node is OFFLINE (set by the app from diag/) */
} param_entry_t;

typedef struct {
    uint32_t value_rejected;                 /* Check 8 failures in received blocks */
    uint32_t blocks[COMM_NODE_COUNT];        /* Blocks applied, per sender */
    uint32_t last_gap_ms[COMM_NODE_COUNT];   /* Arrival gap between that sender's last two blocks */
    uint32_t worst_gap_ms[COMM_NODE_COUNT];  /* Largest gap since boot */
    uint32_t held[COMM_NODE_COUNT];          /* Blocks that arrived while that sender was settling */
} params_stats_t;

/* All 20 NEVER_RECEIVED and STALE. Call once before anything else. */
void params_init(void);

/* Stores a new sample of one of this node's own parameters. Check 8 runs on it: a value that
 * fails keeps the old one, flagged INVALID and sent as SOURCE_FAULT. Returns false for an id
 * this node does not own or a value that fails. */
bool params_set_local(uint8_t id, param_value_t value, uint32_t now_ms);

/* This node's five values and their status word, ready for comm_send_param_block(). */
void params_build_block(comm_param_block_t *out);

/* Applies a block that comm/ accepted (checks 1-7 passed) from node. Check 8 runs per value:
 * source status, NaN/infinity, min/max. A value that fails keeps the old one, flagged INVALID;
 * the others in the block are still applied. Blocks claiming this node's own parameters are
 * ignored. A block more than 30 ms after the sender's previous one starts its settling: its
 * parameters stay STALE until a block arrives 5-30 ms after the previous one (open item 2). */
void params_apply_block(uint8_t node, const comm_param_block_t *block, uint32_t seq, uint32_t rx_time_ms);

/* The 5 ms freshness task (§6.2): updates the stored freshness of every parameter and the
 * source status of our own, and logs every FRESH/STALE and VALID/INVALID change. */
void params_update_freshness(uint32_t now_ms);

/* Copy of parameter id (1-20). Returns false for any other id. */
bool params_get(uint8_t id, uint32_t now_ms, param_entry_t *out);

uint8_t params_synced_count(void); /* Parameters that are VALID and FRESH, 0-20 */

/* True when all five parameters of node (1-4, not this node) are VALID and FRESH here. diag/
 * uses it to tell a node whose heartbeats stopped but whose data still arrives (NO_HEARTBEAT)
 * from one that is OFFLINE. */
bool params_node_synced(uint8_t node);

/* Flags node's five parameters as "owner OFFLINE". The app copies diag/'s
 * view here; params/ itself never decides it. Their values, validity and freshness are kept.
 * Each change is logged. Ignored for this node and for 0/5+. */
void params_set_owner_offline(uint8_t node, bool offline, uint32_t now_ms);

/* Goes up by one on every change the UART link reports as an event: a parameter's validity,
 * its freshness, or its owner-offline flag. With diag_change_count(), tells MCU1 when to look
 * for events (events option b). 0 after params_init(). */
uint32_t params_change_count(void);
uint32_t params_value_rejected(void);
void params_get_stats(params_stats_t *out);

/* Two lines for the debug log: counts, arrival gaps and held blocks, then all 20 raw values. */
void params_debug_report(void);

#endif
