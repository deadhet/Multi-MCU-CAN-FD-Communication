#include "stm32h743xx.h"
#include "drivers/cycles.h"

#define DWT_UNLOCK_KEY 0xC5ACCE55U /* Cortex-M7: the DWT ignores software writes until unlocked */

void cycles_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* Powers the DWT (and ITM) */
    DWT->LAR = DWT_UNLOCK_KEY;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t cycles_now(void)
{
    return DWT->CYCCNT;
}
