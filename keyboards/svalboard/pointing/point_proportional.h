// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t  position[2];  // Q16 reference counts, relative to the activation origin
    int64_t  remainder[2]; // sub-unit wheel integration only; never whole-report debt
    int8_t   direction[2];
    uint32_t last_output;
    uint8_t  axes;
} sval_proportional_t;

void sval_proportional_reset(sval_proportional_t *s, uint32_t now);
void sval_proportional_move(sval_proportional_t *s, int32_t h, int32_t v);
// Sets the position outright (a TrackPoint's deflection, Q16 reference counts).
void sval_proportional_set(sval_proportional_t *s, int64_t h, int64_t v);
void sval_proportional_discard_output(sval_proportional_t *s, uint32_t now);
bool sval_proportional_displaced(const sval_proportional_t *s, uint8_t dead_zone);
// Full 2D position is retained while axes gates OUTPUT. The dead zone is circular
// in free mode and projected onto the admitted axis in locked mode.
void sval_proportional_output(sval_proportional_t *s, uint16_t horizontal, uint16_t vertical, uint8_t dead_zone, uint8_t interval, uint8_t axes, uint8_t inverted, uint8_t host_divisor, uint16_t wheel_units_per_step, uint32_t now, int16_t out[2]);
