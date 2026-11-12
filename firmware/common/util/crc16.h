#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

/* CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR.
 * Check value: "123456789" gives 0x29B1. Design Document §5.5. */
#define CRC16_INIT  0xFFFFU
#define CRC16_CHECK 0x29B1U

/* Continues a CRC over len more bytes. Start with CRC16_INIT. */
uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len);

#endif
