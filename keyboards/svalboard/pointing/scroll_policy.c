// SPDX-License-Identifier: GPL-2.0-or-later
#include "scroll_policy.h"
#include <stdlib.h>

// Extracted from keymap_support.c. Preserve asymmetric acquisition, strict idle
// boundary and transition discard behavior. Call cadence belongs to the adapter.
bool sval_scroll_policy_update(sval_scroll_policy_t *s, int32_t h, int32_t v, uint16_t now) {
    if (h || v) {
        s->last_motion = now;
    } else {
        if ((uint16_t)(now - s->last_motion) > 500 && s->mode != SV_AXIS_FREE) {
            s->mode = SV_AXIS_FREE;
            s->h = s->v = 0;
            return true;
        }
        return false;
    }
    switch (s->mode) {
        case SV_AXIS_FREE:
            s->h += h;
            s->v += v;
            if (abs(s->v) * 3 >= abs(s->h) * 2) {
                s->mode = SV_AXIS_LOCKED_V;
                s->h = 0;
            } else if (abs(s->h) >= 6250) {
                s->mode = SV_AXIS_LOCKED_H;
                s->v = 0;
            }
            break;
        case SV_AXIS_LOCKED_V:
            if (abs(v) * 3 >= abs(h) * 2) s->h = 0;
            s->h += h;
            if (abs(s->h) >= 18750) {
                s->mode = SV_AXIS_LOCKED_H;
                s->v = 0;
                return true;
            }
            break;
        case SV_AXIS_LOCKED_H:
            if (abs(h) * 3 >= abs(v) * 2) s->v = 0;
            s->v += v;
            if (abs(s->v) >= 18750) {
                s->mode = SV_AXIS_LOCKED_V;
                s->h = 0;
                return true;
            }
            break;
    }
    return false;
}
uint8_t sval_scroll_policy_axes(const sval_scroll_policy_t *s, bool enabled) {
    // Legacy FREE state in enabled mode outputs H, not both axes.
    return !enabled ? SVAL_AXIS_BOTH : s->mode == SV_AXIS_LOCKED_V ? SVAL_AXIS_V : SVAL_AXIS_H;
}
