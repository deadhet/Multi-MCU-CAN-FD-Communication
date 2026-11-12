/* UART link protocol (Design Document §8), MCU1 only. Encodes SNAPSHOT, EVENT and CONFIG messages
 * with nanopb (schema: proto/monitor.proto), wraps each in a frame and hands it to the UART link:
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
    uint32_t events;              /* Handed to the link's event queue */
    uint32_t events_dropped;      /* Refused: the event queue was full */
    uint32_t skipped;             /* Frames never sent: replaced by a newer one, or events
                                   * dropped (= frames_skipped) */
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

/* EVENT frames (§8.3, grouped; Het 12 Nov 2026). Compares the view with the one seen at the
 * previous call (at start-up: the boot state) and sends one event per kind of change, naming
 * as a bit mask every node or parameter that made it (bit n-1 = MCUn or Pn). Order: peers that
 * restarted; peers whose link changed, one event per new link state; MCU1's own state; then
 * parameters: owner went offline, owner back, became VALID, became INVALID, became FRESH,
 * became STALE. Called every 5 ms, so a change that reverts within one 5 ms step is not seen.
 * Events wait in a queue and are never replaced; if it is full, an event is lost and counted
 * (events_dropped, frames_skipped). Returns the number of events made. */
uint32_t monitor_send_events(const monitor_view_t *view);

void monitor_get_stats(monitor_stats_t *out);

/* Controllers online out of 4: this node, plus every peer whose link is ONLINE. A peer that is
 * LATE, OFFLINE, RECOVERING, NO_HEARTBEAT, DATA_INVALID or never heard does not count. */
uint32_t monitor_online_count(const monitor_view_t *view);

/* Network health, 0-100 (Het, 12 Nov 2026; assignment dashboard Section 1): the weaker of
 *   controllers online       online / 4
 *   parameters synchronised  synced / 20   (VALID and FRESH at MCU1, params_synced_count())
 * each in whole percent, rounded down. Never better than the weakest part. Examples:
 *   4/4, 20/20 -> 100    3/4, 15/20 -> 75    4/4, 15/20 -> 75    4/4, 0/20 -> 0
 * MCU1 sends it in every snapshot and prints it in its 1 s report, from the same functions. */
uint32_t monitor_health_percent(uint32_t online, uint32_t synced);

#endif
