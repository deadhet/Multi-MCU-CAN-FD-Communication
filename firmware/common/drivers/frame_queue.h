#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

/* Queue of received frames between one producer (the FDCAN interrupt) and one consumer (the
 * main loop). It keeps the newest frames: when it is full, the producer simply overwrites the
 * oldest slot, and the consumer finds out afterwards (lap detection). No interrupt is ever
 * masked, and each index has a single writer.
 *
 *   head: frames written so far. Only the producer writes it.
 *   tail: frames taken or given up so far. Only the consumer writes it.
 *   Both count up forever; slot = index & (FRAME_QUEUE_DEPTH - 1); waiting = head - tail.
 *
 * The producer must not be interrupted by the consumer (true for an ISR and the main loop on
 * one core), and there must be only one consumer per queue. */

#include <stdbool.h>
#include <stdint.h>
#include "drivers/fdcan.h"

#define FRAME_QUEUE_DEPTH 8U /* Must be a power of two */

_Static_assert((FRAME_QUEUE_DEPTH & (FRAME_QUEUE_DEPTH - 1U)) == 0U, "depth must be a power of two");

typedef struct {
    can_rx_slot_t slots[FRAME_QUEUE_DEPTH];
    volatile uint32_t head; /* Producer only */
    volatile uint32_t tail; /* Consumer only */
    uint32_t overwritten;   /* Consumer only: frames given up because the producer lapped */
} frame_queue_t;

/* Consumer side, before the producer starts: empties the queue and clears its counter. */
void frame_queue_reset(frame_queue_t *q);

/* Producer side: the slot the next frame goes into. Fill it, then call frame_queue_commit(). */
can_rx_slot_t *frame_queue_next_slot(frame_queue_t *q);
void frame_queue_commit(frame_queue_t *q);

/* Consumer side: copies the oldest frame still intact into out. Returns false if empty. */
bool frame_queue_pop(frame_queue_t *q, can_rx_slot_t *out);

#ifdef FRAME_QUEUE_TEST_HOOK
/* Host tests only: called between the copy and the check, to run the producer mid-copy. */
void FRAME_QUEUE_TEST_HOOK(frame_queue_t *q);
#endif

#endif
