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

/* Received frames are sorted into this many RAM queues. Each keeps its newest frames, so a
 * flood on one queue cannot push frames out of another. */
#define FDCAN_RX_QUEUES 4U

/* One hardware filter: standard IDs first_id to last_id go to Rx FIFO 0, and from there into
 * RAM queue `queue` (0 to FDCAN_RX_QUEUES - 1). */
typedef struct {
    uint16_t first_id;
    uint16_t last_id;
    uint8_t queue;
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
    uint32_t rx_fifo_lost;                    /* Hardware Rx FIFO overflowed (events) */
    uint32_t rx_overwritten[FDCAN_RX_QUEUES]; /* Oldest frames given up in each RAM queue */
    uint32_t rx_unrouted;                     /* Accepted by hardware but no range matched */
    uint32_t tx_fifo_full;                    /* fdcan_send() found the Tx FIFO full */
} fdcan_stats_t;

/* Returns false if the controller did not respond, the filter list is too long, or a range
 * names a queue that does not exist. */
bool fdcan_init(fdcan_mode_t mode, const fdcan_id_range_t *ranges, uint32_t range_count);

/* Queues one FD frame with BRS and returns at once. Returns false if the Tx FIFO is full
 * or len is not a valid CAN FD length. */
bool fdcan_send(uint32_t id, const uint8_t *data, uint8_t len);

/* Copies the oldest frame still held in RAM queue `queue` into out. Returns false if that queue
 * is empty or does not exist. Main loop only: one reader per queue. */
bool fdcan_receive(uint32_t queue, can_rx_slot_t *out);

void fdcan_get_stats(fdcan_stats_t *out);

/* The configured bit rates and BRS, for the UART link's CONFIG message. Computed from the same
 * constants that set NBTP and DBTP: Renode does not keep writes to those registers, so they
 * cannot be read back there (docs/setup_findings.md). */
uint32_t fdcan_nominal_bitrate(void);
uint32_t fdcan_data_bitrate(void);
bool fdcan_brs_enabled(void);

#endif
