#include "monitor/cobs.h"

/* Each group is a code byte followed by up to 254 data bytes. The code is the group's length
 * plus one; a code below 0xFF also stands for a 0x00 that followed the group in the input. */
uint32_t cobs_encode(const uint8_t *in, uint32_t len, uint8_t *out)
{
    uint32_t code_pos = 0U; /* Where the current group's code byte goes */
    uint32_t out_pos = 1U;
    uint8_t code = 1U;

    for (uint32_t i = 0U; i < len; i++) {
        if (in[i] == 0U) {
            out[code_pos] = code;
            code_pos = out_pos++;
            code = 1U;
        } else {
            out[out_pos++] = in[i];
            code++;
            if (code == 0xFFU) { /* 254 data bytes: the group is full */
                out[code_pos] = code;
                code_pos = out_pos++;
                code = 1U;
            }
        }
    }
    out[code_pos] = code;
    return out_pos;
}
