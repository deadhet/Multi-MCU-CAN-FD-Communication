/* Overwriting frame queue with lap detection. See frame_queue.h for the rules. */
#include "drivers/frame_queue.h"

#if defined(__arm__)
#include "cmsis_compiler.h"
#define QUEUE_BARRIER() __DMB() /* PM0253 p.177; also stops the compiler moving accesses */
#else
#define QUEUE_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST) /* Host test build only */
#endif

/* Host tests define this to run the producer in the middle of a copy. Empty in firmware. */
#ifndef FRAME_QUEUE_TEST_HOOK
#define FRAME_QUEUE_TEST_HOOK(q)
#endif

#define SLOT_MASK (FRAME_QUEUE_DEPTH - 1U)

void frame_queue_reset(frame_queue_t *q)
{
    q->head = 0U;
    q->tail = 0U;
    q->overwritten = 0U;
}

can_rx_slot_t *frame_queue_next_slot(frame_queue_t *q)
{
    /* No "full" check: when full, this is the oldest slot, and the newest frame wins. */
    return &q->slots[q->head & SLOT_MASK];
}

void frame_queue_commit(frame_queue_t *q)
{
    uint32_t head = q->head;

    QUEUE_BARRIER(); /* The slot must be complete before the new head is visible */
    q->head = head + 1U;
}

/* Frame t lives in slot t & mask until the producer writes frame t + DEPTH into the same slot,
 * which moves head past t + DEPTH. The producer always runs to completion before the consumer
 * continues, so "head - t <= DEPTH" means frame t is still intact. */
bool frame_queue_pop(frame_queue_t *q, can_rx_slot_t *out)
{
    bool got = false;
    bool done = false;

    while (!done) {
        uint32_t tail = q->tail;
        uint32_t head = q->head;

        if (head == tail) {
            done = true; /* Empty */
        } else {
            if ((head - tail) > FRAME_QUEUE_DEPTH) {
                /* Lapped: the oldest frames were overwritten. Skip to the oldest one left. */
                q->overwritten += (head - tail) - FRAME_QUEUE_DEPTH;
                tail = head - FRAME_QUEUE_DEPTH;
            }

            QUEUE_BARRIER(); /* Read the slot only after reading head */
            *out = q->slots[tail & SLOT_MASK];
            FRAME_QUEUE_TEST_HOOK(q);
            QUEUE_BARRIER(); /* Finish the copy before reading head again */

            if ((q->head - tail) > FRAME_QUEUE_DEPTH) {
                /* The producer reused this slot during the copy: the copy may be torn. Keep the
                 * new tail and go round again; the next pass counts the lost frame. */
                q->tail = tail;
            } else {
                q->tail = tail + 1U;
                got = true;
                done = true;
            }
        }
    }
    return got;
}
