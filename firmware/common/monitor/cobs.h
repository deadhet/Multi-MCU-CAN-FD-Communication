/* COBS, Consistent Overhead Byte Stuffing (Design Document §8.2). Removes every 0x00 from a
 * frame, so the link can use 0x00 to mark frame ends: after any error, the next 0x00 is always
 * a true frame end. The overhead is fixed and small: at most 1 byte per 254, plus 1. */
#ifndef COBS_H
#define COBS_H

#include <stdint.h>

/* Largest encoded size for len input bytes, without the 0x00 terminator. */
#define COBS_MAX_ENCODED(len) ((len) + ((len) / 254U) + 1U)

/* Encodes len bytes of in into out (which needs COBS_MAX_ENCODED(len) bytes) and returns the
 * encoded length. The output contains no 0x00; the caller appends the terminator. */
uint32_t cobs_encode(const uint8_t *in, uint32_t len, uint8_t *out);

#endif
