/* UART link protocol (Design Document §8), MCU1 only. Encodes SNAPSHOT and CONFIG messages with
 * nanopb (schema: proto/monitor.proto), wraps each in a frame and queues it on the UART link:
 *   [version << 4 | type][UART sequence, uint32 LE][Protobuf payload][CRC-16 LE] -> COBS -> 0x00
 * It calls no other service (§4.4): the app hands it copies (monitor_view_t). Called from the
 * main loop only. */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdbool.h>
#include <stdint.h>
#include "comm/comm.h"
#include "diag/diag.h"
#include "params/params.h"

#define MONITOR_PROTOCOL_VERSION 1U

/* Frame types, the low nibble of the first byte. */
#define MONITOR_FRAME_SNAPSHOT 1U
#define MONITOR_FRAME_EVENT    2U
#define MONITOR_FRAME_CONFIG   3U

/* §8.4: a snapshot is queued only if this much stays free after it, for events and CONFIG. */
#define MONITOR_RESERVE_BYTES 256U

/* What the app collects for a snapshot: copies, never pointers into other modules. */
typedef struct {
    uint32_t now_ms;
    comm_node_state_t own_state;
    uint8_t synced_count;                /* params_synced_count() */
    uint16_t own_error_summary;          /* as in this node's heartbeat */
    param_entry_t params[PARAM_COUNT];   /* P1 first */
    diag_peer_t peers[COMM_NODE_COUNT];  /* index node - 1; the entry for this node is unused */
} monitor_view_t;

/* What the app knows about timing, for CONFIG. */
typedef struct {
    uint32_t param_period_ms;
    uint32_t heartbeat_period_ms;
    uint32_t snapshot_period_ms;
} monitor_periods_t;

typedef struct {
    uint32_t snapshots;           /* Queued */
    uint32_t configs;             /* Queued */
    uint32_t skipped;             /* Snapshots not queued: no room (§8.4), or an encode error */
    uint32_t refused;             /* CONFIG not queued: no room */
    uint32_t encode_errors;       /* nanopb refused to encode (should never happen) */
    uint32_t last_snapshot_bytes; /* On the wire, terminator included */
    uint32_t max_snapshot_bytes;
} monitor_stats_t;

void monitor_init(void);

/* Encodes, frames and queues one SNAPSHOT. False if it was skipped or could not be encoded. */
bool monitor_send_snapshot(const monitor_view_t *view);

/* Encodes, frames and queues one CONFIG. False if it did not fit or could not be encoded. */
bool monitor_send_config(const monitor_periods_t *periods);

void monitor_get_stats(monitor_stats_t *out);

#endif
