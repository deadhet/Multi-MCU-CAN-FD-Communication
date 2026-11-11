/* P1-P20 definitions. Section numbers refer to the Design Document. */
#include <float.h>
#include <stddef.h>
#include "system_table.h"

/* §6.3: the owner reports SOURCE_STALE when its own sample is older than 3 x its sample period. */
#define SOURCE_STALE_FACTOR 3U

/* Ranges are each type's full domain for now (Het, 11 Nov 2026), so check 8 only rejects what
 * can never be a value: a float NaN or infinity, a bool other than 0 or 1. Real limits per
 * parameter go here later; nothing else changes. */
#define FLOAT_ANY .min = { .f = -FLT_MAX }, .max = { .f = FLT_MAX }
#define INT_ANY   .min = { .i = INT32_MIN }, .max = { .i = INT32_MAX }
#define UINT_ANY  .min = { .u = 0U }, .max = { .u = UINT32_MAX }
#define BOOL_ANY  .min = { .u = 0U }, .max = { .u = 1U }

#define PARAM(id_, owner_, type_, range_, period_ms_)                                          \
    { .id = (id_), .name = "Param " #id_, .owner = (owner_), .type = (type_), range_,           \
      .sample_period_ms = (period_ms_), .source_stale_ms = SOURCE_STALE_FACTOR * (period_ms_) }

/* Every node uses the same 5-slot layout (Het, 11 Nov 2026), so each type and each sample
 * period runs on all four nodes. */
#define NODE_PARAMS(owner_, p1, p2, p3, p4, p5)                \
    PARAM(p1, owner_, PARAM_TYPE_FLOAT32, FLOAT_ANY, 10U),     \
    PARAM(p2, owner_, PARAM_TYPE_FLOAT32, FLOAT_ANY, 50U),     \
    PARAM(p3, owner_, PARAM_TYPE_INT32, INT_ANY, 100U),        \
    PARAM(p4, owner_, PARAM_TYPE_UINT32, UINT_ANY, 500U),      \
    PARAM(p5, owner_, PARAM_TYPE_BOOL, BOOL_ANY, 1000U)

static const param_def_t table[PARAM_COUNT] = {
    NODE_PARAMS(1U, 1, 2, 3, 4, 5),
    NODE_PARAMS(2U, 6, 7, 8, 9, 10),
    NODE_PARAMS(3U, 11, 12, 13, 14, 15),
    NODE_PARAMS(4U, 16, 17, 18, 19, 20),
};

const param_def_t *system_table_get(uint8_t id)
{
    if ((id < 1U) || (id > PARAM_COUNT)) {
        return NULL;
    }
    return &table[id - 1U];
}
