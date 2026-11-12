/* Latest-wins transmit slots for the UART link (Design Document §8.4 as revised by Het,
 * 12 Nov 2026). Logic only, no hardware, so the PC tests run it too; drivers/uart_link.c adds
 * the hardware.
 *
 * One slot per kind of frame (SNAPSHOT, CONFIG), each with two buffers:
 *   - the mover (the UART interrupt now; a DMA on hardware later) may be sending one of them;
 *   - the writer (main loop) writes the next frame of that kind into the other, then hands it
 *     over. A frame handed over but not started yet is "pending".
 * If a newer frame of the same kind comes while one is still pending, the pending one is taken
 * back and overwritten: only the most recent frame of each kind ever waits. When the frame on
 * its way out is finished ("the DMA is empty"), the mover takes the pending frame that was handed
 * over first, so frames leave in the order they were made.
 *
 * Events are different: each one records a change and must not be replaced by the next. They
 * wait in a queue (a ring of fixed-size entries) and go out in the same hand-over order as the
 * slots' frames. When the queue is full, a new event is refused (and counted by the caller).
 *
 * Contexts: the writer calls the *_begin() and *_commit() functions; the mover calls
 * tx_slots_done() and tx_slots_next(). The mover is an interrupt, which the main loop never
 * interrupts. The one thing both can try at the same moment is taking a pending slot frame;
 * that is an atomic exchange, so exactly one of them gets it. The queue needs none: only the
 * writer moves its tail, only the mover its head. No interrupt is ever masked. */
#ifndef TX_SLOTS_H
#define TX_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TX_SLOT_SNAPSHOT = 0,
    TX_SLOT_CONFIG,
    TX_SLOT_COUNT
} tx_slot_t;

typedef struct {
    uint32_t handed[TX_SLOT_COUNT];   /* Frames handed over by the writer */
    uint32_t replaced[TX_SLOT_COUNT]; /* Pending frames taken back unsent, for a newer one */
    uint32_t queued;                  /* Events handed over to the queue */
    uint32_t queue_full;              /* Events refused: the queue was full */
    uint32_t queue_peak;              /* Most entries in use at once, the one being sent included */
    uint32_t sent;                    /* Frames the mover finished, events included */
    uint32_t bytes;                   /* Their bytes */
} tx_slots_stats_t;

/* Entries in the event queue: a power of two. */
#define TX_QUEUE_ENTRIES 16U

/* Forgets every slot and frame. Call before tx_slots_attach(), with the mover stopped. */
void tx_slots_init(void);

/* Gives a slot its two buffers, of size bytes each. Before the first tx_slots_begin(). */
void tx_slots_attach(tx_slot_t slot, uint8_t *buf_a, uint8_t *buf_b, uint32_t size);

/* Writer. The buffer to write the next frame of this kind into; *size is its size. If a frame
 * of this kind was still pending, it is taken back (it will never be sent), counted, and
 * *replaced is true. NULL if the slot has no buffers. */
uint8_t *tx_slots_begin(tx_slot_t slot, uint32_t *size, bool *replaced);

/* Writer. Hands over the len bytes written since tx_slots_begin() for this slot. */
void tx_slots_commit(tx_slot_t slot, uint32_t len);

/* Gives the event queue its buffer: TX_QUEUE_ENTRIES entries of entry_size bytes each, one after
 * the other. Before the first tx_slots_queue_begin(). */
void tx_slots_attach_queue(uint8_t *entries, uint32_t entry_size);

/* Writer. The entry to write the next event into; *size is its size. NULL, counted in
 * queue_full, if the queue is full or has no buffer. */
uint8_t *tx_slots_queue_begin(uint32_t *size);

/* Writer. Hands over the len bytes written since tx_slots_queue_begin(). */
void tx_slots_queue_commit(uint32_t len);

/* Mover. The frame from tx_slots_next() is completely handed to the hardware: its buffer is free
 * again. Does nothing if no frame was being sent. */
void tx_slots_done(void);

/* Mover. Takes the pending frame or queued event handed over first and marks it as being sent.
 * False if nothing is waiting. */
bool tx_slots_next(const uint8_t **data, uint32_t *len);

void tx_slots_get_stats(tx_slots_stats_t *out);

#endif
