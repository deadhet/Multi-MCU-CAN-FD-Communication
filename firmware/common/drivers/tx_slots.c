#include <stdatomic.h>
#include <stddef.h>
#include "drivers/tx_slots.h"

#define NO_BUFFER 0xFFU

typedef struct {
    uint8_t *buf[2];
    uint32_t size;
    uint32_t len[2];         /* Set by the writer before the hand-over */
    uint32_t order[2];       /* Hand-over number, set by the writer before the hand-over */
    uint8_t writing;         /* Writer only: the buffer given out by tx_slots_begin() */
    _Atomic uint8_t pending; /* The pending buffer (0 or 1), or NO_BUFFER */
} slot_state_t;

static slot_state_t slots[TX_SLOT_COUNT];
static volatile uint8_t sending;  /* Mover only: slot * 2 + buffer, or NO_BUFFER */
static uint32_t sending_len;      /* Mover only */
static uint32_t handovers;        /* Writer only: numbers the hand-overs, for the order */
static tx_slots_stats_t stats;    /* handed, replaced: writer only. sent, bytes: mover only */

_Static_assert(ATOMIC_CHAR_LOCK_FREE == 2, "the exchange must not need a lock");

void tx_slots_init(void)
{
    for (uint32_t k = 0U; k < (uint32_t)TX_SLOT_COUNT; k++) {
        slots[k] = (slot_state_t){ 0 };
        atomic_init(&slots[k].pending, NO_BUFFER);
    }
    sending = NO_BUFFER;
    sending_len = 0U;
    handovers = 0U;
    stats = (tx_slots_stats_t){ 0 };
}

void tx_slots_attach(tx_slot_t slot, uint8_t *buf_a, uint8_t *buf_b, uint32_t size)
{
    slots[slot].buf[0] = buf_a;
    slots[slot].buf[1] = buf_b;
    slots[slot].size = size;
}

uint8_t *tx_slots_begin(tx_slot_t slot, uint32_t *size, bool *replaced)
{
    slot_state_t *s = &slots[slot];

    *replaced = false;
    *size = 0U;
    if (s->buf[0] == NULL) {
        return NULL;
    }

    /* Take back a frame of this kind that is still pending. The exchange reads `pending` and
     * leaves NO_BUFFER in one step that an interrupt cannot split: if the mover takes the frame
     * first, this sees NO_BUFFER and the frame goes out. */
    uint8_t taken = atomic_exchange_explicit(&s->pending, NO_BUFFER, memory_order_acquire);
    if (taken != NO_BUFFER) {
        *replaced = true;
        stats.replaced[slot]++;
        s->writing = taken;
    } else {
        /* Nothing of this kind is pending, and the mover only ever starts a pending frame, so
         * the buffer of this slot that is not being sent is free. */
        s->writing = (sending == (uint8_t)((uint32_t)slot * 2U)) ? 1U : 0U;
    }
    *size = s->size;
    return s->buf[s->writing];
}

void tx_slots_commit(tx_slot_t slot, uint32_t len)
{
    slot_state_t *s = &slots[slot];
    uint8_t w = s->writing;

    s->len[w] = len;
    s->order[w] = ++handovers;
    stats.handed[slot]++;
    /* Release: the frame's bytes, length and order are in memory before the mover can see it. */
    atomic_store_explicit(&s->pending, w, memory_order_release);
}

void tx_slots_done(void)
{
    if (sending != NO_BUFFER) {
        stats.sent++;
        stats.bytes += sending_len;
        sending = NO_BUFFER;
    }
}

bool tx_slots_next(const uint8_t **data, uint32_t *len)
{
    uint32_t best = (uint32_t)TX_SLOT_COUNT;
    uint32_t best_order = 0U;

    for (uint32_t k = 0U; k < (uint32_t)TX_SLOT_COUNT; k++) {
        uint8_t p = atomic_load_explicit(&slots[k].pending, memory_order_acquire);
        /* The oldest hand-over first. Signed difference: right even after the count wraps. */
        if ((p != NO_BUFFER) && ((best == (uint32_t)TX_SLOT_COUNT) || ((int32_t)(slots[k].order[p] - best_order) < 0))) {
            best = k;
            best_order = slots[k].order[p];
        }
    }
    if (best == (uint32_t)TX_SLOT_COUNT) {
        return false;
    }

    /* The main loop cannot run until this interrupt returns, so `pending` still holds the
     * buffer just seen. It is still taken by exchange: a writer stopped half-way through its
     * own exchange then finds it changed and starts again. */
    uint8_t p = atomic_exchange_explicit(&slots[best].pending, NO_BUFFER, memory_order_acquire);
    if (p == NO_BUFFER) {
        return false; /* Cannot happen (see above); never send from a buffer nobody handed over */
    }
    sending = (uint8_t)((best * 2U) + p);
    sending_len = slots[best].len[p];
    *data = slots[best].buf[p];
    *len = sending_len;
    return true;
}

void tx_slots_get_stats(tx_slots_stats_t *out)
{
    *out = stats;
}
