/* FDCAN1 driver, register level. Design notes and the calculations are in
 * docs/fdcan_design.md. Page numbers refer to RM0433 Rev 9.
 *
 * This driver moves frames and knows nothing about what is inside them. */
#include <stddef.h>
#include "stm32h743xx.h"
#include "drivers/clock.h"
#include "drivers/fdcan.h"
#include "drivers/systick.h"

#define WAIT_TRIES 1000000U

/* Bit timing: 80 MHz / 2 = 40 MHz, so one time quantum is 25 ns in both phases. */
#define BIT_PRESCALER 2U

#define NOMINAL_TSEG1 63U
#define NOMINAL_TSEG2 16U
#define NOMINAL_SJW   16U
#define NOMINAL_TQ    (1U + NOMINAL_TSEG1 + NOMINAL_TSEG2)

#define DATA_TSEG1 14U
#define DATA_TSEG2 5U
#define DATA_SJW   5U
#define DATA_TQ    (1U + DATA_TSEG1 + DATA_TSEG2)

_Static_assert(CLOCK_FDCAN_HZ / BIT_PRESCALER / NOMINAL_TQ == 500000U, "Nominal bitrate must be 500 kbit/s");
_Static_assert(CLOCK_FDCAN_HZ / BIT_PRESCALER / DATA_TQ == 2000000U, "Data bitrate must be 2 Mbit/s");

/* Secondary sample point at the data sample point, in kernel clock periods. */
#define TDC_OFFSET ((1U + DATA_TSEG1) * BIT_PRESCALER)

/* Message RAM layout, byte offsets from SRAMCAN_BASE. */
#define MAX_FILTERS   8U
#define FILTER_SIZE   4U
#define RX_FIFO_DEPTH 16U
#define TX_FIFO_DEPTH 4U
#define ELEMENT_SIZE  (8U + CAN_MAX_DATA)
#define DATA_SIZE_64  7U /* Element size code for 64 data bytes */

#define FILTER_OFFSET 0U
#define RX_OFFSET     (FILTER_OFFSET + (MAX_FILTERS * FILTER_SIZE))
#define TX_OFFSET     (RX_OFFSET + (RX_FIFO_DEPTH * ELEMENT_SIZE))
#define RAM_USED      (TX_OFFSET + (TX_FIFO_DEPTH * ELEMENT_SIZE))

/* Standard filter element, p.2516. */
#define FILTER_TYPE_RANGE     (0U << 30)
#define FILTER_STORE_FIFO0    (1U << 27)
#define FILTER_ID1_POS        16U

/* Global filter: 2 = reject frames that match no filter, p.2539. */
#define REJECT_NON_MATCHING 2U

/* Rx and Tx element words, p.2513. */
#define ELEMENT_ID_POS   18U
#define ELEMENT_ID_MASK  0x7FFU
#define ELEMENT_DLC_POS  16U
#define ELEMENT_DLC_MASK 0xFU
#define ELEMENT_BRS      (1U << 20)
#define ELEMENT_FDF      (1U << 21)

#define FDCAN_PRIORITY 1U

#define RX_RING_SIZE 32U /* Must be a power of two */
#define RX_RING_MASK (RX_RING_SIZE - 1U)

_Static_assert(sizeof(can_rx_slot_t) == 76U, "can_rx_slot_t must be 76 bytes");
_Static_assert(offsetof(can_rx_slot_t, data) == 12U, "data must follow the 12-byte header");

static const uint8_t dlc_to_len[16] = { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U };

/* Free-running indices, so all 32 slots are usable. used = head - tail. */
static can_rx_slot_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head; /* Written only by the ISR */
static volatile uint32_t rx_tail; /* Written only by fdcan_receive() */

static volatile uint32_t rx_ring_dropped;
static volatile uint32_t rx_fifo_lost;
static uint32_t tx_fifo_full;

/* Message RAM must be accessed as 32-bit words. */
static volatile uint32_t *ram_word(uint32_t byte_offset)
{
    return (volatile uint32_t *)(SRAMCAN_BASE + byte_offset);
}

static bool wait_flag(const volatile uint32_t *reg, uint32_t mask, uint32_t expected)
{
    for (uint32_t tries = 0U; tries < WAIT_TRIES; tries++) {
        if ((*reg & mask) == expected) {
            return true;
        }
    }
    return false;
}

static bool len_to_dlc(uint8_t len, uint32_t *dlc)
{
    for (uint32_t code = 0U; code < 16U; code++) {
        if (dlc_to_len[code] == len) {
            *dlc = code;
            return true;
        }
    }
    return false;
}

static void set_bit_timing(void)
{
    FDCAN1->NBTP = ((NOMINAL_SJW - 1U) << FDCAN_NBTP_NSJW_Pos)
                 | ((BIT_PRESCALER - 1U) << FDCAN_NBTP_NBRP_Pos)
                 | ((NOMINAL_TSEG1 - 1U) << FDCAN_NBTP_NTSEG1_Pos)
                 | ((NOMINAL_TSEG2 - 1U) << FDCAN_NBTP_NTSEG2_Pos);

    FDCAN1->DBTP = FDCAN_DBTP_TDC
                 | ((BIT_PRESCALER - 1U) << FDCAN_DBTP_DBRP_Pos)
                 | ((DATA_TSEG1 - 1U) << FDCAN_DBTP_DTSEG1_Pos)
                 | ((DATA_TSEG2 - 1U) << FDCAN_DBTP_DTSEG2_Pos)
                 | ((DATA_SJW - 1U) << FDCAN_DBTP_DSJW_Pos);

    FDCAN1->TDCR = TDC_OFFSET << FDCAN_TDCR_TDCO_Pos;
}

static void set_message_ram(const fdcan_id_range_t *ranges, uint32_t range_count)
{
    /* Message RAM keeps its contents across a reset, so start from a known state. */
    for (uint32_t offset = 0U; offset < RAM_USED; offset += 4U) {
        *ram_word(offset) = 0U;
    }

    for (uint32_t i = 0U; i < range_count; i++) {
        *ram_word(FILTER_OFFSET + (i * FILTER_SIZE)) = FILTER_TYPE_RANGE | FILTER_STORE_FIFO0
                                                     | ((uint32_t)ranges[i].first_id << FILTER_ID1_POS)
                                                     | ranges[i].last_id;
    }

    FDCAN1->SIDFC = (range_count << FDCAN_SIDFC_LSS_Pos) | FILTER_OFFSET;
    FDCAN1->XIDFC = 0U;
    FDCAN1->GFC = (REJECT_NON_MATCHING << FDCAN_GFC_ANFS_Pos) | (REJECT_NON_MATCHING << FDCAN_GFC_ANFE_Pos)
                | FDCAN_GFC_RRFS | FDCAN_GFC_RRFE;

    FDCAN1->RXF0C = (RX_FIFO_DEPTH << FDCAN_RXF0C_F0S_Pos) | RX_OFFSET;
    FDCAN1->RXESC = DATA_SIZE_64 << FDCAN_RXESC_F0DS_Pos;

    FDCAN1->TXBC = (TX_FIFO_DEPTH << FDCAN_TXBC_TFQS_Pos) | TX_OFFSET;
    FDCAN1->TXESC = DATA_SIZE_64 << FDCAN_TXESC_TBDS_Pos;
}

bool fdcan_init(fdcan_mode_t mode, const fdcan_id_range_t *ranges, uint32_t range_count)
{
    if (range_count > MAX_FILTERS) {
        return false;
    }

    /* p.2461: configuration needs both INIT and CCE. */
    FDCAN1->CCCR |= FDCAN_CCCR_INIT;
    if (!wait_flag(&FDCAN1->CCCR, FDCAN_CCCR_INIT, FDCAN_CCCR_INIT)) {
        return false;
    }
    FDCAN1->CCCR |= FDCAN_CCCR_CCE;

    FDCAN1->CCCR |= FDCAN_CCCR_FDOE | FDCAN_CCCR_BRSE;

    if (mode == FDCAN_MODE_LOOPBACK) {
        /* p.2468: TEST opens the test register, LBCK and MON select internal loop back. */
        FDCAN1->CCCR |= FDCAN_CCCR_TEST | FDCAN_CCCR_MON;
        FDCAN1->TEST |= FDCAN_TEST_LBCK;
    }

    set_bit_timing();
    set_message_ram(ranges, range_count);

    FDCAN1->IE = FDCAN_IE_RF0NE | FDCAN_IE_RF0LE;
    FDCAN1->ILS = 0U;
    FDCAN1->ILE = FDCAN_ILE_EINT0;

    NVIC_SetPriority(FDCAN1_IT0_IRQn, FDCAN_PRIORITY);
    NVIC_EnableIRQ(FDCAN1_IT0_IRQn);

    FDCAN1->CCCR &= ~FDCAN_CCCR_INIT;
    return wait_flag(&FDCAN1->CCCR, FDCAN_CCCR_INIT, 0U);
}

bool fdcan_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    uint32_t dlc;

    if ((id > ELEMENT_ID_MASK) || !len_to_dlc(len, &dlc)) {
        return false;
    }

    uint32_t status = FDCAN1->TXFQS;

    if ((status & FDCAN_TXFQS_TFQF) != 0U) {
        /* Renode does not restart pending frames after a reconnect, and a new add request is
         * what restarts it. Real hardware ignores a request for a pending buffer (p.2552). */
        FDCAN1->TXBAR = FDCAN1->TXBRP;
        tx_fifo_full++;
        return false;
    }

    uint32_t put = (status & FDCAN_TXFQS_TFQPI) >> FDCAN_TXFQS_TFQPI_Pos;
    uint32_t element = TX_OFFSET + (put * ELEMENT_SIZE);

    *ram_word(element) = id << ELEMENT_ID_POS;
    *ram_word(element + 4U) = (dlc << ELEMENT_DLC_POS) | ELEMENT_BRS | ELEMENT_FDF;

    for (uint32_t i = 0U; i < len; i += 4U) {
        uint32_t word = 0U;

        for (uint32_t byte = 0U; (byte < 4U) && ((i + byte) < len); byte++) {
            word |= (uint32_t)data[i + byte] << (byte * 8U);
        }
        *ram_word(element + 8U + i) = word;
    }

    FDCAN1->TXBAR = 1UL << put;
    return true;
}

bool fdcan_receive(can_rx_slot_t *out)
{
    uint32_t tail = rx_tail;

    if (tail == rx_head) {
        return false;
    }

    __DMB(); /* Read the slot only after seeing the new head. */
    *out = rx_ring[tail & RX_RING_MASK];
    __DMB(); /* Finish the copy before the ISR may reuse the slot. */
    rx_tail = tail + 1U;
    return true;
}

void fdcan_get_stats(fdcan_stats_t *out)
{
    out->rx_ring_dropped = rx_ring_dropped;
    out->rx_fifo_lost = rx_fifo_lost;
    out->tx_fifo_full = tx_fifo_full;
}

static void store_rx_element(uint32_t index)
{
    uint32_t head = rx_head;

    if ((head - rx_tail) >= RX_RING_SIZE) {
        rx_ring_dropped++;
        return;
    }

    uint32_t element = RX_OFFSET + (index * ELEMENT_SIZE);
    uint32_t word0 = *ram_word(element);
    uint32_t word1 = *ram_word(element + 4U);
    can_rx_slot_t *slot = &rx_ring[head & RX_RING_MASK];

    slot->id = (word0 >> ELEMENT_ID_POS) & ELEMENT_ID_MASK;
    slot->rx_time_ms = systick_now_ms();
    slot->len = dlc_to_len[(word1 >> ELEMENT_DLC_POS) & ELEMENT_DLC_MASK];
    slot->flags = (((word1 & ELEMENT_FDF) != 0U) ? CAN_RX_FLAG_FD : 0U)
                | (((word1 & ELEMENT_BRS) != 0U) ? CAN_RX_FLAG_BRS : 0U);

    for (uint32_t i = 0U; i < slot->len; i += 4U) {
        uint32_t word = *ram_word(element + 8U + i);

        for (uint32_t byte = 0U; byte < 4U; byte++) {
            slot->data[i + byte] = (uint8_t)(word >> (byte * 8U));
        }
    }

    __DMB(); /* The slot must be complete before the new head is visible. */
    rx_head = head + 1U;
}

void FDCAN1_IT0_IRQHandler(void)
{
    uint32_t flags = FDCAN1->IR & (FDCAN_IR_RF0N | FDCAN_IR_RF0L);

    FDCAN1->IR = flags; /* Write 1 to clear */

    if ((flags & FDCAN_IR_RF0L) != 0U) {
        rx_fifo_lost++;
    }

    while ((FDCAN1->RXF0S & FDCAN_RXF0S_F0FL) != 0U) {
        uint32_t index = (FDCAN1->RXF0S & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;

        store_rx_element(index);
        FDCAN1->RXF0A = index;
    }
}
