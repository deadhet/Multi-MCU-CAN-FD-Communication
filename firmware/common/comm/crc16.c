#include "comm/crc16.h"

#define CRC16_POLY 0x1021U

/* Bitwise, MSB first, no table: about 8 steps per byte, under 2 us for a 32-byte frame
 * at 480 MHz, and no 512-byte table in flash. */
uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ CRC16_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
