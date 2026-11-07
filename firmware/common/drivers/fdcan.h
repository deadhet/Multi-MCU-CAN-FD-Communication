#ifndef FDCAN_H
#define FDCAN_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_RX_FLAG_FD  0x01U
#define CAN_RX_FLAG_BRS 0x02U
#define CAN_MAX_DATA    64U

typedef enum {
    FDCAN_MODE_NORMAL,
    FDCAN_MODE_LOOPBACK /* Internal loop back, for self test only */
} fdcan_mode_t;

/* One hardware filter: standard IDs first_id to last_id go to Rx FIFO 0. */
typedef struct {
    uint16_t first_id;
    uint16_t last_id;
} fdcan_id_range_t;

typedef struct {
    uint32_t id;
    uint32_t rx_time_ms;
    uint8_t len;
    uint8_t flags; /* CAN_RX_FLAG_FD, CAN_RX_FLAG_BRS */
    uint8_t _pad[2];
    uint8_t data[CAN_MAX_DATA];
} can_rx_slot_t;

typedef struct {
    uint32_t rx_ring_dropped; /* Software ring was full */
    uint32_t rx_fifo_lost;    /* Hardware Rx FIFO overflowed */
    uint32_t tx_fifo_full;    /* fdcan_send() found the Tx FIFO full */
} fdcan_stats_t;

/* Returns false if the controller did not respond or the filter list is too long. */
bool fdcan_init(fdcan_mode_t mode, const fdcan_id_range_t *ranges, uint32_t range_count);

/* Queues one FD frame with BRS and returns at once. Returns false if the Tx FIFO is full
 * or len is not a valid CAN FD length. */
bool fdcan_send(uint32_t id, const uint8_t *data, uint8_t len);

/* Copies the oldest received frame into out. Returns false if there is none. */
bool fdcan_receive(can_rx_slot_t *out);

void fdcan_get_stats(fdcan_stats_t *out);

#endif
