// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { SVAL_AXIS_H = 1, SVAL_AXIS_V = 2, SVAL_AXIS_BOTH = 3 };
typedef enum { SV_AXIS_FREE, SV_AXIS_LOCKED_V, SV_AXIS_LOCKED_H } sval_scroll_mode_t;
typedef struct {
    sval_scroll_mode_t mode;
    int32_t            h, v;
    uint16_t           last_motion;
} sval_scroll_policy_t;
// Returns true exactly when the legacy policy discarded its scroll buffers.
bool    sval_scroll_policy_update(sval_scroll_policy_t *s, int32_t h, int32_t v, uint16_t now);
uint8_t sval_scroll_policy_axes(const sval_scroll_policy_t *s, bool enabled);
