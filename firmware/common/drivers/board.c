#include "stm32h743xx.h"
#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/systick.h"
#include "drivers/uart.h"

#define PIN_MODE_AF 2U
#define USART1_TX_AF 7U

/* USART1 TX on PA9. p.372: read back the enable, then dummy-read the peripheral. */
static void usart1_pins_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    (void)USART1->CR1;

    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (USART1_TX_AF << GPIO_AFRH_AFSEL9_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | (PIN_MODE_AF << GPIO_MODER_MODE9_Pos);
}

void board_init(void)
{
    if (!clock_init()) {
        /* Fail fast: nothing is safe to run on an unknown clock. */
        for (;;) {
        }
    }
    systick_init();
    usart1_pins_init();
    uart_debug_init();
}