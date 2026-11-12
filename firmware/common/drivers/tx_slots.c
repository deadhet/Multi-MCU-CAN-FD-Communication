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

#define SENDING_QUEUE 0xFEU /* `sending` value: the event queue's head entry */
#define QUEUE_MASK    (TX_QUEUE_ENTRIES - 1U)

static slot_state_t slots[TX_SLOT_COUNT];
static volatile uint8_t sending;  /* Mover only: slot * 2 + buffer, SENDING_QUEUE or NO_BUFFER */
static uint32_t sending_len;      /* Mover only */
static uint32_t handovers;        /* Writer only: numbers the hand-overs, for the order */
static tx_slots_stats_t stats;    /* handed, replaced, queued, queue_full, queue_peak: writer only.
                                   * sent, bytes: mover only */

/* Event queue. Entry i is free while it is outside [head, tail). The writer fills the entry at
 * tail and then moves tail; the mover sends the entry at head and moves head only when it is
 * done, so the writer never touches an entry on its way out. Free-running counters: the
 * difference counts the entries in use, also after they wrap. */
static uint8_t *queue_buf;
static uint32_t queue_entry_size;
static uint32_t queue_len[TX_QUEUE_ENTRIES];   /* Set by the writer before the hand-over */
static uint32_t queue_order[TX_QUEUE_ENTRIES]; /* Hand-over number, the same way */
static _Atomic uint32_t queue_tail;            /* Moved by the writer only */
static _Atomic uint32_t queue_head;            /* Moved by the mover only */

_Static_assert(ATOMIC_CHAR_LOCK_FREE == 2, "the exchange must not need a lock");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "the queue counters must not need a lock");
_Static_assert((TX_QUEUE_ENTRIES & QUEUE_MASK) == 0U, "TX_QUEUE_ENTRIES must be a power of two");

void tx_slots_init(void)
{
    for (uint32_t k = 0U; k < (uint32_t)TX_SLOT_COUNT; k++) {
        slots[k] = (slot_state_t){ 0 };
        atomic_init(&slots[k].pending, NO_BUFFER);
    }
    queue_buf = NULL;
    queue_entry_size = 0U;
    atomic_init(&queue_tail, 0U);
    atomic_init(&queue_head, 0U);
    sending = NO_BUFFER;
    sending_len = 0U;
    handovers = 0U;
    stats = (tx_slots_stats_t){ 0 };
}

void tx_slots_attach_queue(uint8_t *entries, uint32_t entry_size)
{
    queue_buf = entries;
    queue_entry_size = entry_size;
}

uint8_t *tx_slots_queue_begin(uint32_t *size)
{
    uint32_t tail = atomic_load_explicit(&queue_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&queue_head, memory_order_acquire);

    *size = 0U;
    if ((queue_buf == NULL) || ((tail - head) >= TX_QUEUE_ENTRIES)) {
        stats.queue_full++;
        return NULL;
    }
    *size = queue_entry_size;
    return &queue_buf[(tail & QUEUE_MASK) * queue_entry_size];
}

void tx_slots_queue_commit(uint32_t len)
{
    uint32_t tail = atomic_load_explicit(&queue_tail, memory_order_relaxed);
    uint32_t i = tail & QUEUE_MASK;

    queue_len[i] = len;
    queue_order[i] = ++handovers;
    stats.queued++;

    /* Entries in use with this one, at the moment the head is read. tx_slots_queue_begin()
     * refused the event if all were in use, so this is at most TX_QUEUE_ENTRIES. */
    uint32_t in_use = (tail + 1U) - atomic_load_explicit(&queue_head, memory_order_relaxed);
    if (in_use > stats.queue_peak) {
        stats.queue_peak = in_use;
    }

    /* Release: the entry's bytes, length and order are in memory before the mover sees it. */
    atomic_store_explicit(&queue_tail, tail + 1U, memory_order_release);
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
        if (sending == SENDING_QUEUE) {
            /* Only now is the entry free for the writer. */
            uint32_t head = atomic_load_explicit(&queue_head, memory_order_relaxed);
            atomic_store_explicit(&queue_head, head + 1U, memory_order_release);
        }
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

    /* The event queue's oldest entry competes in the same order. */
    uint32_t head = atomic_load_explicit(&queue_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&queue_tail, memory_order_acquire);
    if (head != tail) {
        uint32_t i = head & QUEUE_MASK;
        if ((best == (uint32_t)TX_SLOT_COUNT) || ((int32_t)(queue_order[i] - best_order) < 0)) {
            sending = SENDING_QUEUE;
            sending_len = queue_len[i];
            *data = &queue_buf[i * queue_entry_size];
            *len = sending_len;
            return true;
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
