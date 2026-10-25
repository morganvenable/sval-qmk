// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SVAL_POINT_ONE 65536L

typedef struct {
    int8_t   direction;
    int32_t  progress, reverse;
    uint32_t last_motion;
    bool     moved;
} sval_point_turn_t;
typedef struct {
    int32_t steps;
    bool    reversed;
} sval_point_steps_t;
sval_point_steps_t sval_point_turn(sval_point_turn_t *s, int32_t delta, int32_t distance, int32_t reversal, uint32_t now);
// Counts at cpi to Q16.16 counts at 800 CPI, carrying the remainder between reports.
int32_t sval_point_normalize(int16_t delta, uint16_t cpi, int64_t *remainder);
// The same, times a device gain (x16: 16 is 1.0, point_device.h).
int32_t sval_point_normalize_gain(int16_t delta, uint16_t cpi, uint8_t gain_x16, int64_t *remainder);
