/* Node health (Design Document §7). Every node runs it: it tracks each peer's link state from
 * its heartbeats, counts missed heartbeats, restarts and recoveries, and keeps this node's own
 * state (BOOT, RESYNCING, OPERATIONAL) for its heartbeat. It only detects and reports; it takes
 * no fault actions.
 *
 * It calls no other service (§4.4, calls only go downward). The app feeds it heartbeats,
 * parameter blocks and each peer's data status, and acts on what it returns: it forgets a peer
 * that went OFFLINE in comm/ and flags that peer's parameters in params/. Called from the main
 * loop only. Times are passed in, so it also runs on a PC. */
#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stdint.h>
#include "comm/comm.h"

/* A peer as this node sees it (§7.2, plus NO_HEARTBEAT). */
typedef enum {
    DIAG_UNKNOWN = 0,     /* No heartbeat since boot, and 350 ms have not passed yet */
    DIAG_ONLINE = 1,
    DIAG_LATE = 2,        /* No heartbeat for 150 ms */
    DIAG_OFFLINE = 3,     /* 3 heartbeats missed and its parameters are not arriving properly */
    DIAG_RECOVERING = 4,  /* Heartbeat back after OFFLINE, or a restart: not yet confirmed */
    DIAG_NO_HEARTBEAT = 5, /* 3 heartbeats missed, but its parameters still arrive VALID and FRESH */
    DIAG_DATA_INVALID = 6  /* Heartbeats arrive, but no block from it passed checks 1-7 for 350 ms */
} diag_link_t;

/* One peer as handed out by diag_get_peer(). */
typedef struct {
    diag_link_t link;
    bool heard;                   /* A heartbeat received since this node's boot */
    comm_heartbeat_t last;        /* Content of the last heartbeat (its state, synced count, ...) */
    uint32_t last_heartbeat_ms;   /* Arrival time of the last heartbeat */
    uint32_t heartbeat_age_ms;    /* now - last_heartbeat_ms at the call (time since boot if never) */
    uint32_t data_age_ms;         /* now - its last accepted parameter block (time since boot if none) */
    uint32_t missed;              /* Heartbeats missed since the last one; 0 after each heartbeat */
    uint32_t heartbeats;          /* Heartbeats received */
    uint32_t restarts;            /* Restarts seen (comm/'s flag, or uptime went down) */
    uint32_t recoveries;          /* RECOVERING -> ONLINE */
} diag_peer_t;

/* All peers UNKNOWN, own state BOOT. Call once before anything else. */
void diag_init(void);

/* A heartbeat that comm/ accepted. restart is comm_frame_t.restart. */
void diag_on_heartbeat(uint8_t node, const comm_heartbeat_t *hb, bool restart, uint32_t rx_time_ms);

/* A parameter block that comm/ accepted: for the own-state rule and for DATA_INVALID. Rejected
 * frames never count. */
void diag_on_param_block(uint8_t node, uint32_t rx_time_ms);

/* The periodic task: missed heartbeats, LATE, OFFLINE, NO_HEARTBEAT, DATA_INVALID and the own state. data_ok[n - 1]
 * says whether node n's parameters are all VALID and FRESH here (params_node_synced()).
 * Returns a bit mask of the nodes that went OFFLINE in this call (bit n - 1 for node n), so the
 * app can call comm_forget_peer() for each. */
uint8_t diag_update(uint32_t now_ms, const bool data_ok[COMM_NODE_COUNT]);

comm_node_state_t diag_own_state(void); /* For this node's heartbeat */
bool diag_peer_offline(uint8_t node);   /* For params_set_owner_offline() */

/* Goes up by one on every change the UART link reports as an event: a peer's link state, a
 * restart, the own state. MCU1 looks for events only when this or params_change_count() moved
 * (events option b), right after the call that made the change, so even a
 * state that lasts under a millisecond is reported. 0 after diag_init(). */
uint32_t diag_change_count(void);

/* Copy of peer node (1-4, not this node). Returns false otherwise. */
bool diag_get_peer(uint8_t node, uint32_t now_ms, diag_peer_t *out);

/* One line for the debug log: own state, then every peer's link state, the state it last
 * reported, heartbeat age, missed count, restarts and recoveries. */
void diag_debug_report(uint32_t now_ms);

#endif
