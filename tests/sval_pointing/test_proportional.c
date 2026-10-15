#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "point_proportional.h"
#define Q 65536
static int sum(sval_proportional_t *s, int h, int v, int dead, int interval, int axes, int inverted, int divisor, unsigned start, unsigned duration, unsigned dt, int *vertical) {
    int total = 0;
    *vertical = 0;
    int16_t out[2];
    sval_proportional_output(s, h, v, dead, interval, 1, axes, inverted, divisor, 120, start, out);
    for (unsigned t = dt; t <= duration; t += dt) {
        sval_proportional_output(s, h, v, dead, interval, 1, axes, inverted, divisor, 120, start + t, out);
        total += out[0];
        *vertical += out[1];
    }
    return total;
}
int main(void) {
    sval_proportional_t s;
    int                 vertical;
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 71 * Q, 0);
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 0, 1000, 1, &vertical) == 2400 && vertical == 0);
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 135 * Q, 0);
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 0, 1000, 10, &vertical) == 4800); // twice displacement past dead zone
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 71 * Q, 0);
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 0, 1000, 10, &vertical) == 2400);    // cadence independent
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 1000, 1000, 10, &vertical) == 2400); // stationary sensor keeps scrolling
    sval_proportional_move(&s, -71 * Q, 0);
    assert(!sval_proportional_displaced(&s, 7));
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 2000, 1000, 10, &vertical) == 0); // return to origin
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 4 * Q, 4 * Q);
    assert(!sval_proportional_displaced(&s, 7));
    assert(sum(&s, 64, 64, 7, 0, 3, 0, 1, 0, 1000, 1, &vertical) == 0 && vertical == 0);
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 5 * Q, 5 * Q);
    assert(sval_proportional_displaced(&s, 7));
    int diagonal = sum(&s, 64, 64, 7, 0, 3, 0, 1, 0, 1000, 1, &vertical);
    assert(diagonal > 0 && diagonal == vertical); // circular, not square
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 64 * Q, 64 * Q);
    assert(sum(&s, 64, 32, 0, 0, 3, 0, 1, 0, 1000, 1, &vertical) == 2400 && vertical == 4800); // independent gains
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 71 * Q, 71 * Q);
    assert(sum(&s, 64, 64, 7, 0, 1, 0, 1, 0, 1000, 10, &vertical) == 2400 && vertical == 0);
    assert(s.position[1] == 71 * Q); // gated travel retains the original physical center
    assert(sum(&s, 64, 64, 7, 0, 2, 0, 1, 1000, 1000, 10, &vertical) == 0 && vertical == 2400);
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 71 * Q, 0);
    assert(sum(&s, 64, 64, 7, 0, 3, 1, 12, 0, 1000, 1, &vertical) == -200); // inversion, host scaling
    sval_proportional_reset(&s, 0);
    sval_proportional_move(&s, 71 * Q, 0);
    assert(sum(&s, 64, 64, 7, 100, 3, 0, 1, 0, 1000, 1, &vertical) == 1200); // explicit 10 steps/sec limit
    // Report bounds and arbitrary timer gaps do not overflow or accrue report debt.
    sval_proportional_reset(&s, 0);
    for (int i = 0; i < 1000; ++i)
        sval_proportional_move(&s, INT32_MAX, INT32_MIN + 1);
    int16_t out[2];
    sval_proportional_output(&s, 8, 8, 0, 0, 0, 3, 0, 1, 120, 0, out);
    sval_proportional_output(&s, 8, 8, 0, 0, 0, 3, 0, 1, 120, UINT32_MAX, out);
    assert(out[0] == 32767 && out[1] == -32767 && s.remainder[0] == 0 && s.remainder[1] == 0);
    sval_proportional_reset(&s, UINT32_MAX - 10);
    sval_proportional_move(&s, 64 * Q, 0);
    sval_proportional_output(&s, 64, 64, 0, 0, 1, 3, 0, 1, 120, UINT32_MAX - 10, out);
    sval_proportional_output(&s, 64, 64, 0, 0, 1, 3, 0, 1, 120, 9, out);
    assert(out[0] == 48); // clock wrap, 20ms
    puts("proportional: linearity, cadence, neutral, circular dead zone, 2D gains, locks, direction, explicit cap, report bounds and clock wrap passed");
}
