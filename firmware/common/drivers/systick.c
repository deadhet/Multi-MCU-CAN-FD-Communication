#include "stm32h743xx.h"
#include "drivers/clock.h"
#include "drivers/systick.h"

#define TICKS_PER_MS (CLOCK_SYSCLK_HZ / 1000U)
#define SYSTICK_PRIORITY 2U

static volatile uint32_t ms_counter;

void SysTick_Handler(void)
{
    ms_counter++;
}

void systick_init(void)
{
    SysTick->LOAD = TICKS_PER_MS - 1U;
    SysTick->VAL = 0U;
    NVIC_SetPriority(SysTick_IRQn, SYSTICK_PRIORITY);
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

uint32_t systick_now_ms(void)
{
    return ms_counter;
}