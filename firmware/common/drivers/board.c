#include "stm32h743xx.h"
#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/systick.h"
#include "drivers/uart.h"

#define PIN_MODE_AF 2U
#define USART1_TX_AF 7U
#define USART3_TX_AF 7U
#define FDCAN1_AF 9U
/* Medium: 60 MHz at 3.3 V and 50 pF without the I/O compensation cell (DS p.141, Table 53).
 * CAN FD at 2 Mbit/s needs 1 MHz. */
#define PIN_SPEED_MEDIUM 1U
#define FDCAN_CLOCK_PLL2Q 2U

/* USART1 TX on PA9. p.372: read back the enable, then dummy-read the peripheral. */
static void usart1_pins_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    //bit 4 is USART1EN to turn on clock peripherals for USART1
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    // following lines are here to make sure the write to this register is being done
    (void)RCC->AHB4ENR;
    (void)RCC->APB2ENR;
    (void)GPIOA->MODER;
    (void)USART1->CR1;

    // GPIOA->AFR[1] = (GPIOA->AFR[1] & ~0x000000F0UL) | (0x7UL << 4);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9) | (USART1_TX_AF << GPIO_AFRH_AFSEL9_Pos);
    // GPIOA->MODER  = (GPIOA->MODER  & ~0x000C0000UL) | (0x2UL << 18);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE9) | (PIN_MODE_AF << GPIO_MODER_MODE9_Pos);
}

/* FDCAN1 on PD0 (RX) and PD1 (TX), AF9. Kernel clock from PLL2 Q, RM0433 p.413. */
static void fdcan1_pins_init(void)
{
    RCC->D2CCIP1R = (RCC->D2CCIP1R & ~RCC_D2CCIP1R_FDCANSEL) | (FDCAN_CLOCK_PLL2Q << RCC_D2CCIP1R_FDCANSEL_Pos);

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
    RCC->APB1HENR |= RCC_APB1HENR_FDCANEN;
    (void)RCC->AHB4ENR;
    (void)RCC->APB1HENR;
    (void)GPIOD->MODER;
    (void)FDCAN1->CCCR;

    // GPIOD->AFR[0]  = (GPIOD->AFR[0]  & ~0x000000FFUL) | (0x9UL << 0) | (0x9UL << 4);
    GPIOD->AFR[0] = (GPIOD->AFR[0] & ~(GPIO_AFRL_AFSEL0 | GPIO_AFRL_AFSEL1))
                  | (FDCAN1_AF << GPIO_AFRL_AFSEL0_Pos) | (FDCAN1_AF << GPIO_AFRL_AFSEL1_Pos); 
    // GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~0x0000000CUL) | (0x2UL << 2);
    GPIOD->OSPEEDR = (GPIOD->OSPEEDR & ~GPIO_OSPEEDR_OSPEED1) | (PIN_SPEED_MEDIUM << GPIO_OSPEEDR_OSPEED1_Pos);
    
    // GPIOD->MODER   = (GPIOD->MODER   & ~0x0000000FUL) | (0x2UL << 0) | (0x2UL << 2);
    GPIOD->MODER = (GPIOD->MODER & ~(GPIO_MODER_MODE0 | GPIO_MODER_MODE1))
                 | (PIN_MODE_AF << GPIO_MODER_MODE0_Pos) | (PIN_MODE_AF << GPIO_MODER_MODE1_Pos);
}

/* UART link (Design Document §8.1): USART3 TX on PD8, AF7. Transmit only, so no RX pin. The
 * kernel clock is left at its reset choice, the APB1 clock (CLOCK_PCLK_HZ). MCU1 only: called by
 * uart_link_init(), not by board_init(). */
void board_uart_link_pins_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
    (void)RCC->AHB4ENR;
    (void)RCC->APB1LENR;
    (void)GPIOD->MODER;
    (void)USART3->CR1;

    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~GPIO_AFRH_AFSEL8) | (USART3_TX_AF << GPIO_AFRH_AFSEL8_Pos);
    GPIOD->MODER = (GPIOD->MODER & ~GPIO_MODER_MODE8) | (PIN_MODE_AF << GPIO_MODER_MODE8_Pos);
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
    fdcan1_pins_init();
}