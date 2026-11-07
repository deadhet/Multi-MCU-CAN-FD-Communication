/* System clock setup. Page numbers refer to RM0433 Rev 9.
 *
 * Target: HSI 64 MHz -> /4 -> 16 MHz PLL input
 *         PLL1: 16 x 60 = 960 MHz VCO, /2 = 480 MHz core
 *         PLL2: 16 x 20 = 320 MHz VCO, /4 = 80 MHz FDCAN clock
 *
 * 480 MHz needs VOS0, which needs silicon revision V and a junction temperature of
 * 105 C or less (DS12110 Rev 11, p.210).
 *
 * The supply configuration is already done in Reset_Handler. */
#include <stdint.h>
#include "stm32h743xx.h"
#include "drivers/clock.h"

#define WAIT_TRIES 1000000U

#define PLL_DIVM  4U  /* Real value goes in the register */
#define PLL_RANGE 3U  /* 8-16 MHz input, p.402 */
#define PLL1_N    60U /* Register holds value - 1, p.404 */
#define PLL1_P    2U  /* Register holds value - 1 */
#define PLL2_N    20U
#define PLL2_Q    4U

#define VOS_SCALE1 3U /* p.310 */

/* Bounded wait: returns false instead of hanging if the flag never arrives. */
static bool wait_flag(const volatile uint32_t *reg, uint32_t mask, uint32_t expected)
{
    for (uint32_t tries = 0U; tries < WAIT_TRIES; tries++) {
        if ((*reg & mask) == expected) {
            return true;
        }
    }
    return false;
}

bool clock_init(void)
{
    /* Step A (worked example): raise the core voltage before raising the clock. p.310.
     * Pattern: clear the field, set the new value, wait for hardware to confirm.
     * VOS0 can only be reached from VOS1, so this is two stages. p.280. */
    PWR->D3CR = (PWR->D3CR & ~PWR_D3CR_VOS) | (VOS_SCALE1 << PWR_D3CR_VOS_Pos);
    if (!wait_flag(&PWR->D3CR, PWR_D3CR_VOSRDY, PWR_D3CR_VOSRDY)) {
        return false;
    }

    /* p.372: read back the enable bit, then dummy-read the peripheral before using it. */
    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    (void)RCC->APB4ENR;
    (void)SYSCFG->PWRCR;

    SYSCFG->PWRCR |= SYSCFG_PWRCR_ODEN;
    if (!wait_flag(&PWR->D3CR, PWR_D3CR_VOSRDY, PWR_D3CR_VOSRDY)) {
        return false;
    }

    /* TODO Step B: flash wait states, before the clock gets faster. Table 17, p.160.
     * 240 MHz AXI at VOS0 needs LATENCY = 4 and WRHIGHFREQ = 2.
     * Clear FLASH_ACR_LATENCY and FLASH_ACR_WRHIGHFREQ, then set FLASH_ACR_LATENCY_4WS
     * and FLASH_ACR_WRHIGHFREQ_1. Use wait_flag() on FLASH->ACR to confirm the latency. */
    
    //FLASH->ACR = (0x37 & ~(0x0F | 0x30)) | 0x04 | 0x20;
    FLASH->ACR = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ)) | FLASH_ACR_LATENCY_4WS | FLASH_ACR_WRHIGHFREQ_1;
    if (!wait_flag(&FLASH->ACR, FLASH_ACR_LATENCY, FLASH_ACR_LATENCY_4WS)) {
        return false;
    }

    /* TODO Step C: PLL input. RCC->PLLCKSELR, p.398.
     * Clear RCC_PLLCKSELR_PLLSRC, RCC_PLLCKSELR_DIVM1 and RCC_PLLCKSELR_DIVM2. Set
     * RCC_PLLCKSELR_PLLSRC_HSI, then PLL_DIVM at DIVM1_Pos and DIVM2_Pos. Keep DIVM3. */

    //RCC->PLLCKSELR = (0x02020200 & ~(0x3 | 0x3F0 | 0x3F000)) | 0 | 0x40 | 0x4000
    RCC->PLLCKSELR = (RCC->PLLCKSELR & ~(RCC_PLLCKSELR_PLLSRC | RCC_PLLCKSELR_DIVM1 | RCC_PLLCKSELR_DIVM2))
               | RCC_PLLCKSELR_PLLSRC_HSI
               | (PLL_DIVM << RCC_PLLCKSELR_DIVM1_Pos)
               | (PLL_DIVM << RCC_PLLCKSELR_DIVM2_Pos);
    
    /* TODO Step D: PLL options. RCC->PLLCFGR, p.400. Assign the whole register:
     * PLL_RANGE at RCC_PLLCFGR_PLL1RGE_Pos and RCC_PLLCFGR_PLL2RGE_Pos, plus
     * RCC_PLLCFGR_DIVP1EN and RCC_PLLCFGR_DIVQ2EN. Everything else off.
     * VCOSEL stays 0: 960 MHz and 320 MHz are both in the wide 192-960 MHz range. */
    // RCC->PLLCFGR = (3 << 2) | (3 << 6) | 0x00010000 | 0x00100000;
    RCC->PLLCFGR = (PLL_RANGE << RCC_PLLCFGR_PLL1RGE_Pos)
             | (PLL_RANGE << RCC_PLLCFGR_PLL2RGE_Pos)
             | RCC_PLLCFGR_DIVP1EN
             | RCC_PLLCFGR_DIVQ2EN;

    /* TODO Step E: PLL dividers, p.403 and p.406. Written as value - 1.
     * Assign each whole register, so unused outputs get 0 as p.347 requires.
     * RCC->PLL1DIVR = (PLL1_N - 1U) at N1_Pos, (PLL1_P - 1U) at P1_Pos.
     * RCC->PLL2DIVR = (PLL2_N - 1U) at N2_Pos, (PLL2_Q - 1U) at Q2_Pos.
     * These fields lock once a PLL is on, so this must come before Step G. */
    // RCC->PLL1DIVR = (59 << 0) | (1 << 9); [16 × 60 ÷ 2 = 480 MHz]
    RCC->PLL1DIVR = ((PLL1_N - 1U) << RCC_PLL1DIVR_N1_Pos) | ((PLL1_P - 1U) << RCC_PLL1DIVR_P1_Pos);

    // RCC->PLL2DIVR = (19 << 0) | (3 << 16); [16 × 20 ÷ 4 = 80 MHz]
    RCC->PLL2DIVR = ((PLL2_N - 1U) << RCC_PLL2DIVR_N2_Pos) | ((PLL2_Q - 1U) << RCC_PLL2DIVR_Q2_Pos);

    /* TODO Step F: bus prescalers, p.394-397. Set while still slow.
     * RCC->D1CFGR: clear RCC_D1CFGR_HPRE and RCC_D1CFGR_D1PPRE, set the _DIV2 values.
     * Leave D1CPRE alone: it divides the CPU clock itself.
     * RCC->D2CFGR: same for D2PPRE1 and D2PPRE2. RCC->D3CFGR: same for D3PPRE. */

    // RCC->D1CFGR = (0 & ~(0x0F | 0x70)) | 0x08 | 0x40;
    RCC->D1CFGR = (RCC->D1CFGR & ~(RCC_D1CFGR_HPRE | RCC_D1CFGR_D1PPRE)) | RCC_D1CFGR_HPRE_DIV2 | RCC_D1CFGR_D1PPRE_DIV2;
    // RCC->D2CFGR = (0 & ~(0x70 | 0x700)) | 0x40 | 0x400;
    RCC->D2CFGR = (RCC->D2CFGR & ~(RCC_D2CFGR_D2PPRE1 | RCC_D2CFGR_D2PPRE2)) | RCC_D2CFGR_D2PPRE1_DIV2 | RCC_D2CFGR_D2PPRE2_DIV2;
    // RCC->D3CFGR = (0 & ~0x70) | 0x40;
    RCC->D3CFGR = (RCC->D3CFGR & ~RCC_D3CFGR_D3PPRE) | RCC_D3CFGR_D3PPRE_DIV2;

    /* TODO Step G: start both PLLs. RCC->CR, p.383.
     * Set RCC_CR_PLL1ON and RCC_CR_PLL2ON, then wait_flag() on RCC_CR_PLL1RDY and
     * RCC_CR_PLL2RDY. Return false if either fails. */
    
    RCC->CR |= RCC_CR_PLL1ON | RCC_CR_PLL2ON;

    if (!wait_flag(&RCC->CR, RCC_CR_PLL1RDY, RCC_CR_PLL1RDY)) {
        return false;
    }
    if (!wait_flag(&RCC->CR, RCC_CR_PLL2RDY, RCC_CR_PLL2RDY)) {
        return false;
    }

    /* TODO Step H: switch the system clock. RCC->CFGR, p.391.
     * Clear RCC_CFGR_SW, set RCC_CFGR_SW_PLL1, then wait_flag() on RCC->CFGR with
     * mask RCC_CFGR_SWS and expected RCC_CFGR_SWS_PLL1. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL1;

    if (!wait_flag(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL1)) {
        return false;
    }
    return true;
}
