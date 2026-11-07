/* Reset handler and vector table for the STM32H743.
 *
 * RAM keeps its old contents across a reset in Renode, so .data and .bss are always
 * initialised here. VTOR resets to 0, so it is always set here too. */
#include <stdint.h>
#include "stm32h743xx.h"

#define CORE_VECTORS 16
#define VECTOR_COUNT (CORE_VECTORS + WAKEUP_PIN_IRQn + 1)
#define IRQ_SLOT(irq) (CORE_VECTORS + (irq))
#define SUPPLY_WAIT_TRIES 1000000U

#ifndef NODE_ID
#error "NODE_ID must be passed by the build"
#endif
_Static_assert(NODE_ID >= 1 && NODE_ID <= 4, "NODE_ID must be 1..4");

typedef void (*vector_t)(void);

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);

void Reset_Handler(void);
void power_supply_init(void);
void system_start(void);

/* Stops here on any interrupt without a handler, so the fault is visible in a debugger. */
void Default_Handler(void)
{
    for (;;) {
    }
}

/* Weak, so a driver overrides one by defining a function with the same name. */
#define WEAK_HANDLER __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_HANDLER;
void HardFault_Handler(void) WEAK_HANDLER;
void MemManage_Handler(void) WEAK_HANDLER;
void BusFault_Handler(void) WEAK_HANDLER;
void UsageFault_Handler(void) WEAK_HANDLER;
void SVC_Handler(void) WEAK_HANDLER;
void DebugMon_Handler(void) WEAK_HANDLER;
void PendSV_Handler(void) WEAK_HANDLER;
void SysTick_Handler(void) WEAK_HANDLER;
void FDCAN1_IT0_IRQHandler(void) WEAK_HANDLER;
void FDCAN1_IT1_IRQHandler(void) WEAK_HANDLER;
void USART1_IRQHandler(void) WEAK_HANDLER;
void USART3_IRQHandler(void) WEAK_HANDLER;

/* Lets a tool read which node an image was built for. */
__attribute__((section(".node_id"), used)) static const uint8_t image_node_id = NODE_ID;

/* Every slot defaults to Default_Handler; the named entries then replace their slot. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) static const vector_t vector_table[VECTOR_COUNT] = {
    [2 ... VECTOR_COUNT - 1] = Default_Handler,
    [0] = (vector_t)&_estack,
    [1] = Reset_Handler,
    [2] = NMI_Handler,
    [3] = HardFault_Handler,
    [4] = MemManage_Handler,
    [5] = BusFault_Handler,
    [6] = UsageFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [IRQ_SLOT(FDCAN1_IT0_IRQn)] = FDCAN1_IT0_IRQHandler,
    [IRQ_SLOT(FDCAN1_IT1_IRQn)] = FDCAN1_IT1_IRQHandler,
    [IRQ_SLOT(USART1_IRQn)] = USART1_IRQHandler,
    [IRQ_SLOT(USART3_IRQn)] = USART3_IRQHandler,
};
#pragma GCC diagnostic pop

/* RM0433 6.4.1: set the supply configuration and wait for valid voltage before any RAM
 * write. This must stay a leaf function so the compiler never uses the stack here. */
__attribute__((used)) void power_supply_init(void)
{
    PWR->CR3 = (PWR->CR3 & ~(PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS)) | PWR_CR3_LDOEN;

    for (uint32_t tries = 0U; (PWR->CSR1 & PWR_CSR1_ACTVOSRDY) == 0U; tries++) {
        if (tries == SUPPLY_WAIT_TRIES) {
            for (;;) {
            }
        }
    }
}

/* Naked, because a normal C function may push to the stack on entry, and the stack is RAM. */
__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile(
        "bl power_supply_init\n"
        "b system_start\n");
}

__attribute__((used)) void system_start(void)
{
    /* The FPU must be on before any compiled code can use it. */
    SCB->CPACR |= (0xFUL << 20);
    __DSB();
    __ISB();

    SCB->VTOR = (uint32_t)vector_table;

    const uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) {
        *dst++ = *src++;
    }
    for (uint32_t *dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0U;
    }

    (void)main();

    for (;;) {
    }
}
