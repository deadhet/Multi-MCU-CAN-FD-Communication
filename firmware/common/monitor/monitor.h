/* UART link protocol (Design Document §8), MCU1 only. Encodes SNAPSHOT and CONFIG messages with
 * nanopb (schema: proto/monitor.proto), wraps each in a frame and hands it to the UART link:
 *   [version << 4 | type][UART sequence, uint32 LE][Protobuf payload][CRC-16 LE] -> COBS -> 0x00
 * Latest wins (§8.4, Het, 12 Nov 2026): while the link is busy, a newer frame of a kind replaces
 * the older one still waiting; the replaced frame's sequence number is never sent, and the
 * snapshot's frames_skipped counts it, so the host can tell such gaps from bytes lost on the
 * way. Its buffers are in .noinit RAM (util/noinit.h).
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
    uint32_t snapshots;           /* Handed to the link */
    uint32_t configs;             /* Handed to the link */
    uint32_t skipped;             /* Frames replaced unsent by a newer one (= frames_skipped) */
    uint32_t encode_errors;       /* Not encoded, so not sent (should never happen) */
    uint32_t last_snapshot_bytes; /* On the wire, terminator included */
    uint32_t max_snapshot_bytes;
} monitor_stats_t;

/* After uart_link_init(): gives the link its slot buffers. */
void monitor_init(void);

/* Encodes, frames and hands over one SNAPSHOT; it replaces a snapshot still waiting. False if
 * it could not be encoded. */
bool monitor_send_snapshot(const monitor_view_t *view);

/* Encodes, frames and hands over one CONFIG; it replaces a CONFIG still waiting. False if it
 * could not be encoded. */
bool monitor_send_config(const monitor_periods_t *periods);

void monitor_get_stats(monitor_stats_t *out);

#endif
