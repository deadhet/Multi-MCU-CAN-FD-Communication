/* P1-P20 definitions, shared by every node (Design Document §5.4, §6.1). This is the static
 * part of the parameter database: it lives in flash and never changes at run time. Every node
 * needs all 20 entries, because it must know the owner and type of each parameter it receives. */
#ifndef SYSTEM_TABLE_H
#define SYSTEM_TABLE_H

#include <stdint.h>

#define PARAM_COUNT     20U
#define PARAMS_PER_NODE 5U

typedef enum {
    PARAM_TYPE_FLOAT32 = 0,
    PARAM_TYPE_INT32 = 1,
    PARAM_TYPE_UINT32 = 2,
    PARAM_TYPE_BOOL = 3 /* Sent as uint32 0 or 1 */
} param_type_t;

/* One 4-byte value slot. Which member is valid comes from the table's type, never from the data. */
typedef union {
    float f;
    int32_t i;
    uint32_t u; /* uint32 and bool */
} param_value_t;

typedef struct {
    uint8_t id;                /* 1-20 */
    const char *name;          /* "Param 1" ... "Param 20" */
    uint8_t owner;             /* Node 1-4, the only node that writes it */
    param_type_t type;
    param_value_t min;         /* Check 8 range, inclusive, in the member that matches type */
    param_value_t max;
    uint32_t sample_period_ms; /* How often the owner samples it */
    uint32_t source_stale_ms;  /* Owner reports SOURCE_STALE when its sample is older: 3 x period */
} param_def_t;

/* Definition of parameter id (1-20), or NULL for any other id. */
const param_def_t *system_table_get(uint8_t id);

#endif
