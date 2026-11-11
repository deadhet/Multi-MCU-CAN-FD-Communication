/* Node health. Section numbers refer to the Design Document. */
#include "diag/diag.h"
#include "drivers/uart.h"

#if NODE_ID < 1 || NODE_ID > 4
#error "NODE_ID must be 1-4"
#endif

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)
#define NODE_TAG "MCU" NUMBER_TEXT(NODE_ID)

/* §7.2 and §10. A heartbeat is due every 100 ms. The first one missed counts at 150 ms of
 * silence (1.5 x period absorbs the ≈2 ms worst-case delay), the next ones every 100 ms after
 * that, so the third is missed at 350 ms: one lost heartbeat never takes a node offline. */
#define HEARTBEAT_PERIOD_MS 100U
#define FIRST_MISS_MS       150U
#define OFFLINE_MISSED      3U

/* DATA_INVALID (Het, 12 Nov 2026): a peer whose heartbeats arrive but none of whose parameter
 * blocks passed checks 1-7 for this long. Blocks come every 10 ms and its parameters are STALE
 * after 30 ms anyway; the node verdict takes as long as OFFLINE, so a short burst of bad frames
 * never changes it. */
#define DATA_TIMEOUT_MS 350U

typedef struct {
    diag_link_t link;
    bool heard;          /* A heartbeat since boot */
    bool got_block;      /* A parameter block since it last went OFFLINE (own-state rule) */
    bool confirm_needed; /* Went OFFLINE or restarted: ONLINE again only through RECOVERING */
    comm_heartbeat_t last;
    uint32_t last_heartbeat_ms;
    uint32_t last_block_ms; /* Arrival of its last accepted parameter block; 0 = boot */
    uint32_t missed;
    uint32_t heartbeats;
    uint32_t restarts;
    uint32_t recoveries;
} peer_t;

static peer_t peers[COMM_NODE_COUNT];
static comm_node_state_t own_state;

static const char *const link_names[] = {
    [DIAG_UNKNOWN] = "UNKNOWN",
    [DIAG_ONLINE] = "ONLINE",
    [DIAG_LATE] = "LATE",
    [DIAG_OFFLINE] = "OFFLINE",
    [DIAG_RECOVERING] = "RECOVERING",
    [DIAG_NO_HEARTBEAT] = "NO_HEARTBEAT",
    [DIAG_DATA_INVALID] = "DATA_INVALID",
};

static const char *const state_names[] = {
    [COMM_STATE_BOOT] = "BOOT",
    [COMM_STATE_RESYNCING] = "RESYNCING",
    [COMM_STATE_OPERATIONAL] = "OPERATIONAL",
};

static bool is_peer(uint8_t node)
{
    return (node >= 1U) && (node <= COMM_NODE_COUNT) && (node != NODE_ID);
}

/* A reported state outside 0-2 is printed as "?" rather than read past the table. */
static const char *state_name(uint8_t state)
{
    return (state <= (uint8_t)COMM_STATE_OPERATIONAL) ? state_names[state] : "?";
}

static void write_field(const char *name, uint32_t value)
{
    uart_debug_write(name);
    uart_debug_write_u32(value);
}

/* §8.6: one line per change, never per frame. "MCU1 t=5355 diag n3 OFFLINE" */
static void log_peer(uint8_t node, uint32_t t_ms, const char *what)
{
    write_field(NODE_TAG " t=", t_ms);
    write_field(" diag n", node);
    uart_debug_write(" ");
    uart_debug_write(what);
    uart_debug_write("\n");
}

static void set_link(uint8_t node, diag_link_t link, uint32_t t_ms)
{
    peer_t *p = &peers[node - 1U];

    if (p->link != link) {
        p->link = link;
        log_peer(node, t_ms, link_names[link]);
    }
}

static void set_own(comm_node_state_t state, uint32_t t_ms)
{
    if (own_state != state) {
        own_state = state;
        write_field(NODE_TAG " t=", t_ms);
        uart_debug_write(" diag self ");
        uart_debug_write(state_names[state]);
        uart_debug_write("\n");
    }
}

void diag_init(void)
{
    for (uint32_t n = 0U; n < COMM_NODE_COUNT; n++) {
        peers[n] = (peer_t){ .link = DIAG_UNKNOWN };
    }
    own_state = COMM_STATE_BOOT;
}

void diag_on_heartbeat(uint8_t node, const comm_heartbeat_t *hb, bool restart, uint32_t rx_time_ms)
{
    if (!is_peer(node)) {
        return;
    }
    peer_t *p = &peers[node - 1U];

    /* §7.3. comm/ flags a restart only while it still tracks the sender. After OFFLINE it has
     * forgotten it, so a reset during an outage (scenario S2) shows here as a lower uptime. */
    bool restarted = restart || (p->heard && (hb->uptime_ms < p->last.uptime_ms));
    if (restarted) {
        p->restarts++;
        log_peer(node, rx_time_ms, "RESTART");
    }

    p->heard = true;
    p->last = *hb;
    p->last_heartbeat_ms = rx_time_ms;
    p->missed = 0U;
    p->heartbeats++;

    if (restarted || (p->confirm_needed && (p->link != DIAG_RECOVERING))) {
        /* Back after OFFLINE (Het, 11 Nov 2026), or restarted: RECOVERING only. This heartbeat
         * can never also confirm the recovery. After a reconnect, the first heartbeat may be an
         * old one left in the sender's Tx FIFO, sent before it lost the bus and still saying
         * OPERATIONAL (the same cause as Design Document open item 2). Only a later heartbeat
         * that reports OPERATIONAL makes the peer ONLINE. */
        p->confirm_needed = true;
        set_link(node, DIAG_RECOVERING, rx_time_ms);
    } else if (p->link == DIAG_RECOVERING) {
        /* §7.2: recovery confirmed, not merely "talking again". */
        if (hb->state == (uint8_t)COMM_STATE_OPERATIONAL) {
            p->confirm_needed = false;
            p->recoveries++;
            p->link = DIAG_ONLINE;
            log_peer(node, rx_time_ms, "ONLINE recovered");
        }
    } else if (p->link == DIAG_DATA_INVALID) {
        /* Its heartbeats were never the problem: only an accepted block ends DATA_INVALID. */
    } else {
        set_link(node, DIAG_ONLINE, rx_time_ms); /* From UNKNOWN, LATE or NO_HEARTBEAT */
    }

    /* Het, 11 Nov 2026: the first heartbeat from any other node ends BOOT. */
    if (own_state == COMM_STATE_BOOT) {
        set_own(COMM_STATE_RESYNCING, rx_time_ms);
    }
}

void diag_on_param_block(uint8_t node, uint32_t rx_time_ms)
{
    if (!is_peer(node)) {
        return;
    }
    peer_t *p = &peers[node - 1U];

    p->got_block = true;
    p->last_block_ms = rx_time_ms;
    if (p->link == DIAG_DATA_INVALID) {
        set_link(node, DIAG_ONLINE, rx_time_ms); /* Its data passes again */
    }
}

/* Het, 11 Nov 2026. RESYNCING -> OPERATIONAL once every connected peer has sent a parameter
 * block that comm/ accepted (rejected frames never count). OPERATIONAL -> RESYNCING when no
 * peer is connected any more: a node alone never reports OPERATIONAL. Other nodes coming and
 * going do not change this node's own state.
 * Het, 12 Nov 2026: a DATA_INVALID peer is not waited for, and does not count as connected:
 * this node exchanges no data with it. */
static void update_own_state(uint32_t now_ms)
{
    bool connected = false;
    bool all_sent = true;

    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        if (!is_peer(node)) {
            continue;
        }
        const peer_t *p = &peers[node - 1U];
        if ((p->link == DIAG_UNKNOWN) || (p->link == DIAG_OFFLINE) || (p->link == DIAG_DATA_INVALID)) {
            continue;
        }
        connected = true;
        if (!p->got_block) {
            all_sent = false;
        }
    }

    if ((own_state == COMM_STATE_RESYNCING) && connected && all_sent) {
        set_own(COMM_STATE_OPERATIONAL, now_ms);
    } else if ((own_state == COMM_STATE_OPERATIONAL) && !connected) {
        set_own(COMM_STATE_RESYNCING, now_ms);
    } else {
        /* BOOT ends only with a heartbeat (diag_on_heartbeat()) */
    }
}

uint8_t diag_update(uint32_t now_ms, const bool data_ok[COMM_NODE_COUNT])
{
    uint8_t went_offline = 0U;

    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        if (!is_peer(node)) {
            continue;
        }
        peer_t *p = &peers[node - 1U];

        /* Counted from the last heartbeat, or from boot if none came. Unsigned, so it stays
         * right when the ms counter wraps. */
        uint32_t silence = now_ms - p->last_heartbeat_ms;
        p->missed = (silence < FIRST_MISS_MS) ? 0U : (1U + ((silence - FIRST_MISS_MS) / HEARTBEAT_PERIOD_MS));

        if (p->missed >= OFFLINE_MISSED) {
            if (data_ok[node - 1U]) {
                /* Het, 11 Nov 2026: heartbeats stopped, but its parameters still arrive VALID
                 * and FRESH. Not OFFLINE: the node is alive and only its heartbeat path fails. */
                set_link(node, DIAG_NO_HEARTBEAT, now_ms);
            } else if (p->link != DIAG_OFFLINE) {
                p->confirm_needed = true;
                p->got_block = false;
                went_offline |= (uint8_t)(1U << (node - 1U));
                set_link(node, DIAG_OFFLINE, now_ms);
            } else {
                /* Already OFFLINE */
            }
        } else if ((p->missed >= 1U) && ((p->link == DIAG_ONLINE) || (p->link == DIAG_DATA_INVALID))) {
            set_link(node, DIAG_LATE, now_ms);
        } else if ((p->link == DIAG_ONLINE) && ((now_ms - p->last_block_ms) >= DATA_TIMEOUT_MS)) {
            /* Het, 12 Nov 2026: heartbeats arrive, but no block from it passed checks 1-7 for
             * 350 ms (its blocks are rejected, or missing). Alive, data path broken: the mirror
             * of NO_HEARTBEAT. Its parameters keep their own state in params/. */
            set_link(node, DIAG_DATA_INVALID, now_ms);
        } else {
            /* UNKNOWN and RECOVERING wait for OFFLINE; LATE stays LATE until a heartbeat */
        }
    }
    update_own_state(now_ms);
    return went_offline;
}

comm_node_state_t diag_own_state(void)
{
    return own_state;
}

bool diag_peer_offline(uint8_t node)
{
    return is_peer(node) && (peers[node - 1U].link == DIAG_OFFLINE);
}

bool diag_get_peer(uint8_t node, uint32_t now_ms, diag_peer_t *out)
{
    if (!is_peer(node)) {
        return false;
    }
    const peer_t *p = &peers[node - 1U];
    *out = (diag_peer_t){
        .link = p->link,
        .heard = p->heard,
        .last = p->last,
        .last_heartbeat_ms = p->last_heartbeat_ms,
        .heartbeat_age_ms = now_ms - p->last_heartbeat_ms,
        .data_age_ms = now_ms - p->last_block_ms,
        .missed = p->missed,
        .heartbeats = p->heartbeats,
        .restarts = p->restarts,
        .recoveries = p->recoveries,
    };
    return true;
}

/* For example:
 *   MCU1 diag self=OPERATIONAL n2=ONLINE/OPERATIONAL age=5 miss=0 r=0 rec=0 n3=OFFLINE/OPERATIONAL age=1205 miss=11 r=0 rec=0 ...
 * nX=link/state is how this node sees node X, then the state X last reported ("-" if never
 * heard). age is the time since its last heartbeat (since boot if never). */
void diag_debug_report(uint32_t now_ms)
{
    uart_debug_write(NODE_TAG " diag self=");
    uart_debug_write(state_names[own_state]);
    for (uint8_t node = 1U; node <= COMM_NODE_COUNT; node++) {
        if (!is_peer(node)) {
            continue;
        }
        const peer_t *p = &peers[node - 1U];
        write_field(" n", node);
        uart_debug_write("=");
        uart_debug_write(link_names[p->link]);
        uart_debug_write("/");
        uart_debug_write(p->heard ? state_name(p->last.state) : "-");
        write_field(" age=", now_ms - p->last_heartbeat_ms);
        write_field(" miss=", p->missed);
        write_field(" r=", p->restarts);
        write_field(" rec=", p->recoveries);
    }
    uart_debug_write("\n");
}
