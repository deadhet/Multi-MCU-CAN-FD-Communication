/* Simulated sensors for this node's five parameters (Design Document §6.3). */
#ifndef PARAM_SOURCE_H
#define PARAM_SOURCE_H

#include <stdint.h>

/* Samples every own parameter whose sample period has passed and stores it with
 * params_set_local(). The first call samples all five. Call it in the parameter send task,
 * right before params_build_block(), so each block carries the newest samples. */
void param_source_task(uint32_t now_ms);

#endif
