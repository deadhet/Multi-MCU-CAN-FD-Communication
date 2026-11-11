/* CAN FD application protocol: IDs, frame layouts, end-to-end CRC, sequence counters and
 * the receive validation pipeline (Design Document §5). This is the only module that knows
 * the frame format. Called from the main loop only. */
#ifndef COMM_H
#define COMM_H

#include <stdbool.h>
#include <stdint.h>

#define COMM_NODE_COUNT       4U
#define COMM_PARAMS_PER_NODE  5U
#define COMM_PROTOCOL_VERSION 1U

/* Node state sent in byte 2 of the heartbeat. */
typedef enum {
    COMM_STATE_BOOT = 0,
    COMM_STATE_RESYNCING = 1,
    COMM_STATE_OPERATIONAL = 2
} comm_node_state_t;

typedef enum {
    COMM_MSG_PARAM = 0,
    COMM_MSG_HEARTBEAT = 1,
    COMM_MSG_COUNT = 2
} comm_msg_t;

/* Parameter block content. Values are raw 4-byte slots; their types come from the system table. */
typedef struct {
    uint16_t status; /* 2-bit source status per parameter in bits 0-9; bits 10-15 are sent as 0 */
    uint32_t values[COMM_PARAMS_PER_NODE];
} comm_param_block_t;

typedef struct {
    uint8_t state;        /* comm_node_state_t */
    uint8_t synced_count; /* 0-20 */
    uint32_t uptime_ms;
    uint16_t error_summary;
} comm_heartbeat_t;

/* A received frame that passed checks 1-7 of the validation pipeline. */
typedef struct {
    comm_msg_t type;
    uint8_t source;      /* 1-4 */
    uint32_t sequence;
    uint32_t lost_before; /* Frames missed from this stream just before this one */
    uint32_t rx_time_ms;  /* Arrival time, taken in the FDCAN interrupt */
    bool restart;         /* Heartbeat that revealed a restart; the sender was reset (§7.3) */
    union {
        comm_param_block_t param;
        comm_heartbeat_t heartbeat;
    } body;
} comm_frame_t;

/* Per-reason counters (Design Document §5.7). */
typedef struct {
    uint32_t bad_id;         /* Check 1: unknown type or sender outside 1-4 */
    uint32_t duplicate_node; /* Check 1: this node's own ID sent by another node */
    uint32_t bad_format;     /* Check 2: not an FD frame with BRS */
    uint32_t bad_length;     /* Check 3 */
    uint32_t bad_version;    /* Check 4 */
    uint32_t bad_source;     /* Check 5: source byte differs from the ID's sender */
    uint32_t bad_crc;        /* Check 6 */
    uint32_t duplicate;      /* Check 7: same sequence again */
    uint32_t old_seq;        /* Check 7: sequence went back */
    uint32_t lost_frames;    /* Check 7: gap, frames still accepted */
    uint32_t rx_overflow;    /* Lost before comm saw them: hardware FIFO events + RAM queue overwrites */
} comm_errors_t;             /* Check 8 is per value and runs in params/, which counts value_rejected */

typedef struct {
    uint32_t accepted[COMM_MSG_COUNT];
    uint32_t last_seq[COMM_MSG_COUNT];
    bool heard[COMM_MSG_COUNT]; /* false = NEVER_HEARD */
    uint32_t restarts;          /* Restarts seen in this node's heartbeats (§7.3) */
} comm_peer_stats_t;

typedef struct {
    uint32_t sent[COMM_MSG_COUNT];       /* Frames queued in the Tx FIFO */
    uint32_t not_queued[COMM_MSG_COUNT]; /* Tx FIFO full, frame dropped and not counted in sequence */
} comm_tx_stats_t;

/* Checks the CRC against its check value, then starts FDCAN1 with the protocol's filters.
 * Returns false if either fails. */
bool comm_init(void);

/* Pack, seal with the CRC and queue. Return false if the Tx FIFO is full; nothing waits. */
bool comm_send_param_block(const comm_param_block_t *block);
bool comm_send_heartbeat(const comm_heartbeat_t *heartbeat);

/* Takes frames from the CAN RX queues, heartbeats first, until one passes the checks. Returns
 * false when all queues are empty. Call in a loop on every main loop pass. */
bool comm_receive(comm_frame_t *out);

/* Puts both streams from node back to NEVER_HEARD, so its next frame is accepted whatever its
 * sequence. For restart detection and OFFLINE (Design Document §5.6). */
void comm_forget_peer(uint8_t node);

void comm_get_errors(comm_errors_t *out);
uint16_t comm_error_summary(void); /* Saturating sum of comm's counters; the app adds params/' */
bool comm_get_peer_stats(uint8_t node, comm_peer_stats_t *out); /* false for this node or 0/5+ */
void comm_get_tx_stats(comm_tx_stats_t *out);

/* Writes a summary of the counters to the debug log (a few lines, not per frame). */
void comm_debug_report(void);

#endif
