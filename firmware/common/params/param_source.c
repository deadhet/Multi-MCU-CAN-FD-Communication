/* Simulated sensors. Section numbers refer to the Design Document.
 * §6.3: the pattern follows the type (float -> sine, int -> ramp, bool -> toggle), and each
 * parameter is sampled at its own period from system_table.c. Amplitudes and steps grow with
 * NODE_ID, so the four nodes' values differ and a mix-up shows in the logs. */
#include <math.h>
#include <stdbool.h>
#include "params/param_source.h"
#include "params/params.h"
#include "system_table.h"

#if NODE_ID < 1 || NODE_ID > 4
#error "NODE_ID must be 1-4"
#endif

#define FIRST_OWN_ID ((((uint32_t)NODE_ID - 1U) * PARAMS_PER_NODE) + 1U)
#define TWO_PI       6.2831853f

/* Ramp limits stay inside the chosen preview values (int -1000..1000, uint 0..10000). */
#define INT_RAMP_MIN  (-1000)
#define INT_RAMP_MAX  1000
#define INT_RAMP_STEP (10 * (int32_t)NODE_ID)
#define UINT_RAMP_MAX  10000U
#define UINT_RAMP_STEP (100U * (uint32_t)NODE_ID)

static uint32_t last_sample_ms[PARAMS_PER_NODE];
static param_value_t state[PARAMS_PER_NODE]; /* Next ramp or toggle value */
static bool started;

/* Float slots: slot 0 swings ±25 x node over 2 s, slot 1 swings 250 ± 50 x node over 5 s. */
static float sine(uint32_t slot, uint32_t now_ms)
{
    float offset = (slot == 0U) ? 0.0f : 250.0f;
    float amplitude = ((slot == 0U) ? 25.0f : 50.0f) * (float)NODE_ID;
    uint32_t cycle_ms = (slot == 0U) ? 2000U : 5000U;
    float phase = TWO_PI * (float)(now_ms % cycle_ms) / (float)cycle_ms;

    return offset + (amplitude * sinf(phase));
}

static param_value_t next_value(const param_def_t *def, uint32_t slot, uint32_t now_ms)
{
    param_value_t v = { .u = 0U };
    param_value_t *s = &state[slot];

    switch (def->type) {
    case PARAM_TYPE_FLOAT32:
        v.f = sine(slot, now_ms);
        break;
    case PARAM_TYPE_INT32:
        v.i = s->i;
        s->i = (s->i > (INT_RAMP_MAX - INT_RAMP_STEP)) ? INT_RAMP_MIN : (s->i + INT_RAMP_STEP);
        break;
    case PARAM_TYPE_UINT32:
        v.u = s->u;
        s->u = (s->u > (UINT_RAMP_MAX - UINT_RAMP_STEP)) ? 0U : (s->u + UINT_RAMP_STEP);
        break;
    case PARAM_TYPE_BOOL:
        v.u = s->u;
        s->u ^= 1U;
        break;
    default:
        break;
    }
    return v;
}

void param_source_task(uint32_t now_ms)
{
    if (!started) {
        for (uint32_t slot = 0U; slot < PARAMS_PER_NODE; slot++) {
            const param_def_t *def = system_table_get((uint8_t)(FIRST_OWN_ID + slot));
            if (def->type == PARAM_TYPE_INT32) {
                state[slot].i = INT_RAMP_MIN;
            }
        }
    }

    for (uint32_t slot = 0U; slot < PARAMS_PER_NODE; slot++) {
        uint8_t id = (uint8_t)(FIRST_OWN_ID + slot);
        const param_def_t *def = system_table_get(id);

        if (started && ((now_ms - last_sample_ms[slot]) < def->sample_period_ms)) {
            continue;
        }
        last_sample_ms[slot] = now_ms;
        (void)params_set_local(id, next_value(def, slot, now_ms), now_ms); /* The patterns never fail check 8 */
    }
    started = true;
}
