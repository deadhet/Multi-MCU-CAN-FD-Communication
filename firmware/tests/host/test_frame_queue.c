/* PC test for drivers/frame_queue.c. Runs the producer at chosen moments, including in the
 * middle of the consumer's copy, which cannot be forced on the target.
 *   firmware/tests/host/run_host_tests.sh */
#include <stdio.h>
#include <string.h>
#include "drivers/frame_queue.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* Producer: frame k carries k in its ID and a pattern in every data byte. */
static void produce(frame_queue_t *q, uint32_t k)
{
    can_rx_slot_t *s = frame_queue_next_slot(q);

    s->id = k;
    s->len = CAN_MAX_DATA;
    for (uint32_t i = 0U; i < CAN_MAX_DATA; i++) {
        s->data[i] = (uint8_t)(k + i);
    }
    frame_queue_commit(q);
}

static int intact(const can_rx_slot_t *s, uint32_t k)
{
    if ((s->id != k) || (s->len != CAN_MAX_DATA)) {
        return 0;
    }
    for (uint32_t i = 0U; i < CAN_MAX_DATA; i++) {
        if (s->data[i] != (uint8_t)(k + i)) {
            return 0;
        }
    }
    return 1;
}

/* Called by frame_queue_pop() between the copy and the check (FRAME_QUEUE_TEST_HOOK). */
static uint32_t hook_frames; /* How many frames the "interrupt" writes during the next copy */
static uint32_t hook_next;   /* Number of the next frame it writes */
static uint32_t hook_calls;

void test_hook(frame_queue_t *q)
{
    hook_calls++;
    while (hook_frames > 0U) {
        produce(q, hook_next++);
        hook_frames--;
    }
}

static frame_queue_t q;

static void fresh(uint32_t start)
{
    frame_queue_reset(&q);
    q.head = start;
    q.tail = start;
    hook_frames = 0U;
    hook_calls = 0U;
}

static void test_empty(void)
{
    can_rx_slot_t out;

    printf("empty\n");
    fresh(0U);
    CHECK(!frame_queue_pop(&q, &out));
    CHECK(q.overwritten == 0U);
}

static void test_in_order(void)
{
    can_rx_slot_t out;

    printf("3 in, 3 out, in order\n");
    fresh(0U);
    for (uint32_t k = 0U; k < 3U; k++) {
        produce(&q, k);
    }
    for (uint32_t k = 0U; k < 3U; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(!frame_queue_pop(&q, &out));
    CHECK(q.overwritten == 0U);
}

static void test_exactly_full(void)
{
    can_rx_slot_t out;

    printf("exactly full (8): nothing lost\n");
    fresh(0U);
    for (uint32_t k = 0U; k < FRAME_QUEUE_DEPTH; k++) {
        produce(&q, k);
    }
    for (uint32_t k = 0U; k < FRAME_QUEUE_DEPTH; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(q.overwritten == 0U);
}

static void test_lapped(void)
{
    can_rx_slot_t out;

    printf("11 in while not reading: oldest 3 lost, newest 8 out in order\n");
    fresh(0U);
    for (uint32_t k = 0U; k < 11U; k++) {
        produce(&q, k);
    }
    for (uint32_t k = 3U; k < 11U; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(!frame_queue_pop(&q, &out));
    CHECK(q.overwritten == 3U);
}

static void test_write_during_copy_full(void)
{
    can_rx_slot_t out;

    printf("full, one frame arrives during the copy of the oldest: copy discarded, retry\n");
    fresh(0U);
    for (uint32_t k = 0U; k < FRAME_QUEUE_DEPTH; k++) {
        produce(&q, k);
    }
    hook_frames = 1U; /* Frame 8 lands in slot 0 while slot 0 is being copied */
    hook_next = FRAME_QUEUE_DEPTH;
    CHECK(frame_queue_pop(&q, &out) && intact(&out, 1U));
    CHECK(hook_calls == 2U); /* First copy thrown away, second one kept */
    CHECK(q.overwritten == 1U);
    for (uint32_t k = 2U; k <= FRAME_QUEUE_DEPTH; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(!frame_queue_pop(&q, &out));
}

static void test_write_during_copy_not_full(void)
{
    can_rx_slot_t out;

    printf("3 waiting, one frame arrives during the copy: copy kept\n");
    fresh(0U);
    for (uint32_t k = 0U; k < 3U; k++) {
        produce(&q, k);
    }
    hook_frames = 1U;
    hook_next = 3U;
    CHECK(frame_queue_pop(&q, &out) && intact(&out, 0U));
    CHECK(hook_calls == 1U);
    for (uint32_t k = 1U; k < 4U; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(q.overwritten == 0U);
}

static void test_burst_during_copy(void)
{
    can_rx_slot_t out;

    printf("full, 12 frames arrive during one copy: newest 8 survive\n");
    fresh(0U);
    for (uint32_t k = 0U; k < FRAME_QUEUE_DEPTH; k++) {
        produce(&q, k);
    }
    hook_frames = 12U; /* Frames 8-19 */
    hook_next = FRAME_QUEUE_DEPTH;
    for (uint32_t k = 12U; k < 20U; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, k));
    }
    CHECK(!frame_queue_pop(&q, &out));
    CHECK(q.overwritten == 12U);
}

static void test_counter_wrap(void)
{
    can_rx_slot_t out;
    uint32_t start = 0xFFFFFFFCU; /* head and tail pass 2^32 during the test */

    printf("counters wrap past 2^32\n");
    fresh(start);
    for (uint32_t k = 0U; k < 11U; k++) {
        produce(&q, start + k);
    }
    for (uint32_t k = 3U; k < 11U; k++) {
        CHECK(frame_queue_pop(&q, &out) && intact(&out, start + k));
    }
    CHECK(!frame_queue_pop(&q, &out));
    CHECK(q.overwritten == 3U);
}

int main(void)
{
    test_empty();
    test_in_order();
    test_exactly_full();
    test_lapped();
    test_write_during_copy_full();
    test_write_during_copy_not_full();
    test_burst_during_copy();
    test_counter_wrap();
    printf(failures == 0 ? "FRAME QUEUE PASS\n" : "FRAME QUEUE FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
