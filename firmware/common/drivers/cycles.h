/* CPU cycle counter (the Cortex-M7 DWT's CYCCNT), for measuring how long code runs.
 * 480 cycles per µs at 480 MHz. The count wraps every 2^32 cycles (8.9 s), so take only
 * differences of shorter spans, by unsigned subtraction.
 * In Renode the CPU executes a fixed 100 million instructions per second, without the real
 * chip's wait states and caches, so Renode's numbers compare tasks with each other but are not
 * the real chip's times. */
#ifndef CYCLES_H
#define CYCLES_H

#include <stdint.h>
#include "drivers/clock.h"

#define CYCLES_PER_US (CLOCK_SYSCLK_HZ / 1000000U)

void cycles_init(void);
uint32_t cycles_now(void);

#endif
