/* PC test for drivers/tx_slots.c, the latest-wins transmit slots of the UART link.
 *   firmware/tests/host/run_host_tests.sh
 * The writer (main loop) and the mover (interrupt) are played step by step: fixed cases first,
 * then 200 000 random steps checked against the rules after every step. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/tx_slots.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

#define BUF_SIZE 16U
static uint8_t bufs[TX_SLOT_COUNT][2][BUF_SIZE];

/* A frame here is 8 bytes: its number (uint32) then its slot, then 3 bytes of filler. */
static void write_frame(tx_slot_t slot, uint32_t number)
{
    uint32_t size = 0U;
    bool replaced = false;
    uint8_t *b = tx_slots_begin(slot, &size, &replaced);
    memcpy(b, &number, 4);
    b[4] = (uint8_t)slot;
    memset(&b[5], 0xA5, 3);
    tx_slots_commit(slot, 8U);
}

static uint32_t frame_number(const uint8_t *d)
{
    uint32_t n;
    memcpy(&n, d, 4);
    return n;
}

static void setup(void)
{
    tx_slots_init();
    for (uint32_t k = 0U; k < (uint32_t)TX_SLOT_COUNT; k++) {
        tx_slots_attach((tx_slot_t)k, bufs[k][0], bufs[k][1], BUF_SIZE);
    }
}

static void test_fixed_cases(void)
{
    const uint8_t *d = NULL;
    uint32_t len = 0U, size = 0U;
    bool replaced = true;
    tx_slots_stats_t st;

    printf("Fixed cases: no buffers, idle, latest wins, oldest first, never the buffer being sent\n");
    tx_slots_init();
    CHECK(tx_slots_begin(TX_SLOT_SNAPSHOT, &size, &replaced) == NULL && size == 0U && !replaced);

    setup();
    CHECK(!tx_slots_next(&d, &len));       /* Nothing handed over yet */
    tx_slots_done();                       /* Nothing being sent: no effect */
    tx_slots_get_stats(&st);
    CHECK(st.sent == 0U);

    /* Latest wins: three snapshots while the mover is busy elsewhere; only the third waits. */
    write_frame(TX_SLOT_SNAPSHOT, 1U);
    write_frame(TX_SLOT_CONFIG, 2U);
    write_frame(TX_SLOT_SNAPSHOT, 3U);
    write_frame(TX_SLOT_SNAPSHOT, 4U);
    tx_slots_get_stats(&st);
    CHECK(st.handed[TX_SLOT_SNAPSHOT] == 3U && st.replaced[TX_SLOT_SNAPSHOT] == 2U);
    CHECK(st.handed[TX_SLOT_CONFIG] == 1U && st.replaced[TX_SLOT_CONFIG] == 0U);

    /* Oldest hand-over first: CONFIG 2 was handed over before snapshot 4. */
    CHECK(tx_slots_next(&d, &len) && len == 8U && frame_number(d) == 2U && d[4] == TX_SLOT_CONFIG);
    tx_slots_done();
    CHECK(tx_slots_next(&d, &len) && frame_number(d) == 4U && d[4] == TX_SLOT_SNAPSHOT);

    /* While snapshot 4 is being sent, the writer gets the other buffer, twice in a row. */
    const uint8_t *sending = d;
    uint8_t copy[8];
    memcpy(copy, d, 8);
    uint8_t *w = tx_slots_begin(TX_SLOT_SNAPSHOT, &size, &replaced);
    CHECK(w != sending && size == BUF_SIZE && !replaced);
    tx_slots_commit(TX_SLOT_SNAPSHOT, 0U); /* An empty frame, pending... */
    w = tx_slots_begin(TX_SLOT_SNAPSHOT, &size, &replaced);
    CHECK(w != sending && replaced);       /* ...is taken back, and its buffer reused */
    tx_slots_commit(TX_SLOT_SNAPSHOT, 0U);
    write_frame(TX_SLOT_SNAPSHOT, 5U);     /* Replaces the second empty one the same way */
    CHECK(memcmp(sending, copy, 8) == 0);  /* The frame on its way out was never touched */
    tx_slots_done();
    CHECK(tx_slots_next(&d, &len) && frame_number(d) == 5U);
    tx_slots_done();
    CHECK(!tx_slots_next(&d, &len));

    tx_slots_get_stats(&st);
    CHECK(st.sent == 3U && st.bytes == 24U);
    CHECK(st.handed[TX_SLOT_SNAPSHOT] == 6U && st.replaced[TX_SLOT_SNAPSHOT] == 4U); /* 4 and 5 sent */
}

/* Random steps. Rules checked:
 *  1. frames leave in hand-over order (numbers only grow),
 *  2. each frame that leaves is the newest of its kind at the moment the mover takes it,
 *  3. a frame being sent is never changed until it is done,
 *  4. handed = sent + replaced + still pending, per kind, at the end. */
static void test_random(void)
{
    printf("Random: 200000 writer and mover steps, rules checked after every step\n");
    setup();
    srand(7);

    uint32_t number = 0U;
    uint32_t newest[TX_SLOT_COUNT] = { 0 };   /* Newest number handed over per kind */
    uint32_t last_sent = 0U;
    const uint8_t *sending = NULL;
    uint8_t sending_copy[8];
    uint32_t sent_per_kind[TX_SLOT_COUNT] = { 0 };

    for (uint32_t step = 0U; step < 200000U; step++) {
        int r = rand() % 10;
        if (r < 4) {                                  /* Writer: a new frame of a random kind */
            tx_slot_t k = (tx_slot_t)(rand() % TX_SLOT_COUNT);
            write_frame(k, ++number);
            newest[k] = number;
        } else if ((r < 7) && (sending != NULL)) {    /* Mover: the frame in flight is done */
            CHECK(memcmp(sending, sending_copy, 8) == 0); /* rule 3 */
            tx_slots_done();
            sending = NULL;
        } else if (sending == NULL) {                 /* Mover: take the next frame */
            const uint8_t *d = NULL;
            uint32_t len = 0U;
            if (tx_slots_next(&d, &len)) {
                uint32_t n = frame_number(d);
                CHECK(len == 8U);
                CHECK(n > last_sent);                 /* rule 1 */
                CHECK(n == newest[d[4]]);             /* rule 2 */
                last_sent = n;
                sent_per_kind[d[4]]++;
                sending = d;
                memcpy(sending_copy, d, 8);
            }
        }
        if (sending != NULL) {
            CHECK(memcmp(sending, sending_copy, 8) == 0); /* rule 3, after writer steps too */
        }
        if (failures > 20) {
            return;
        }
    }

    /* Finish: send what is left. */
    const uint8_t *d = NULL;
    uint32_t len = 0U;
    tx_slots_done();
    while (tx_slots_next(&d, &len)) {
        sent_per_kind[d[4]]++;
        tx_slots_done();
    }
    tx_slots_stats_t st;
    tx_slots_get_stats(&st);
    for (uint32_t k = 0U; k < (uint32_t)TX_SLOT_COUNT; k++) {
        CHECK(st.handed[k] == sent_per_kind[k] + st.replaced[k]); /* rule 4 (nothing pending now) */
    }
    printf("  %u frames handed over, %u sent, %u replaced\n", (unsigned)number, (unsigned)st.sent,
           (unsigned)(st.replaced[0] + st.replaced[1]));
}

int main(void)
{
    test_fixed_cases();
    test_random();
    printf("%s (%d failures)\n", (failures == 0) ? "TX SLOTS PASS" : "TX SLOTS FAIL", failures);
    return (failures == 0) ? 0 : 1;
}
