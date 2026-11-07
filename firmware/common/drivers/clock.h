#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

#define CLOCK_SYSCLK_HZ 480000000U /* CPU core */
#define CLOCK_HCLK_HZ   240000000U /* AHB and AXI */
#define CLOCK_PCLK_HZ   120000000U /* All APB buses */
#define CLOCK_FDCAN_HZ  80000000U  /* PLL2 Q output */

/* Returns false if a ready flag never came. The clock state is then unknown. */
bool clock_init(void);

#endif
