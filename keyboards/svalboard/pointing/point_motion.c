// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_motion.h"
#include <string.h>
#include <limits.h>

int32_t sval_point_normalize(int16_t delta, uint16_t cpi, int64_t *remainder) {
    if (!cpi) return 0;
    int64_t denominator = cpi;
    int64_t numerator   = (int64_t)delta * 800 * SVAL_POINT_ONE + *remainder;
    *remainder          = numerator % denominator;
    int64_t result      = numerator / denominator;
    // Bound malformed/extreme inputs; valid sensor travel is far below this.
    if (result > INT32_MAX) return INT32_MAX;
    if (result < -INT32_MAX) return -INT32_MAX;
    return result;
}
sval_point_steps_t sval_point_turn(sval_point_turn_t *s, int32_t delta, int32_t distance, int32_t reversal, uint32_t now) {
    sval_point_steps_t result = {0};
    if (s->moved && (uint32_t)(now - s->last_motion) >= 250) memset(s, 0, sizeof(*s));
    if (!delta || distance <= 0 || reversal <= 0 || reversal > distance) return result;
    s->last_motion    = now;
    s->moved          = true;
    int8_t  direction = delta > 0 ? 1 : -1;
    int64_t amount    = delta > 0 ? delta : -(int64_t)delta;
    if (!s->direction) s->direction = direction;
    if (direction != s->direction) {
        amount += s->reverse;
        if (amount < reversal) {
            s->reverse = amount;
            return result;
        }
        s->direction = direction;
        s->progress = s->reverse = 0;
        result.reversed          = true;
    } else {
        int64_t paid = amount < s->reverse ? amount : s->reverse;
        s->reverse -= paid;
        amount -= paid;
    }
    amount += s->progress;
    result.steps = s->direction * (amount / distance);
    s->progress  = amount % distance;
    return result;
}
