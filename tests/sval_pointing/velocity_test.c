// Host test for velocity_scroll.c: the speed curve, the trackball offset and
// its clamp, the touchpad's anchors and pads, axis lock, and smooth output.
// Run by test_detect.py.
#include <math.h>
#include <stdio.h>
#include "velocity_scroll.h"

static int failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                            \
        }                                                          \
    } while (0)
#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

static const sval_vs_curve_t ball = {1.0f, 10.0f, 1.0f, 60.0f};
static const sval_vs_curve_t pad  = {2.0f, 20.0f, 2.0f, 40.0f};

// Wheel units over ms milliseconds, stepped every dt ms.
static void run(sval_vs_t *s, bool lock, int ms, int dt, float units, long *h, long *v) {
    *h = *v = 0;
    for (int t = 0; t < ms; t += dt) {
        int16_t a, b;
        sval_vs_step(s, &ball, &pad, lock, (float)dt, units, &a, &b);
        *h += a;
        *v += b;
    }
}

static void test_curve(void) {
    CHECK(sval_vs_speed(&ball, 0.0f) == 0.0f);
    CHECK(sval_vs_speed(&ball, 1.0f) == 0.0f);              // the edge of the dead zone
    CHECK(NEAR(sval_vs_speed(&ball, 11.0f), 10.0f, 1e-4f)); // gain at 10 mm past it
    CHECK(NEAR(sval_vs_speed(&ball, -6.0f), -5.0f, 1e-4f)); // signed, linear at exponent 1
    CHECK(sval_vs_speed(&ball, 500.0f) == 60.0f);           // capped
    CHECK(NEAR(sval_vs_speed(&pad, 12.0f), 20.0f, 1e-4f));
    CHECK(NEAR(sval_vs_speed(&pad, 7.0f), 5.0f, 1e-4f)); // (5/10)^2 * 20
    CHECK(NEAR(sval_vs_full_mm(&ball), 61.0f, 1e-3f));   // 1 + 10 * 60/10
    CHECK(NEAR(sval_vs_full_mm(&pad), 2.0f + 10.0f * sqrtf(2.0f), 1e-3f));
}

static void test_ball(void) {
    sval_vs_t s;
    long      h, v;
    sval_vs_start(&s);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0);                      // at the anchor: nothing
    sval_vs_add_relative(&s, 0.0f, 11.0f, -1.0f); // 11 mm toward the user
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -1200, 2)); // 10 notches/s down, while the ball is still
    run(&s, false, 1000, 1, 120, &h, &v);
    CHECK(NEAR(v, -1200, 2));                      // cadence does not matter
    sval_vs_add_relative(&s, 0.0f, -11.0f, -1.0f); // rolled back to the anchor
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0);
    // Slow speeds stay smooth: 0.5 notch/s in whole notches still arrives.
    sval_vs_add_relative(&s, 1.5f, 0.0f, -1.0f);
    run(&s, false, 4000, 8, 1, &h, &v);
    CHECK(h == 2 && v == 0);
    // Clamp: rolling far past the point of maximum speed winds up nothing.
    sval_vs_start(&s);
    float full = sval_vs_full_mm(&ball);
    sval_vs_add_relative(&s, 0.0f, 500.0f, full);
    CHECK(s.rel_y == full);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(NEAR(v, -7200, 2));
    sval_vs_add_relative(&s, 0.0f, -30.0f, full); // any roll back slows at once
    CHECK(sval_vs_speed(&ball, s.rel_y) < 60.0f);
    // Stop: nothing more.
    sval_vs_stop(&s);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0 && !s.active);
}

static void test_dead_zone_per_axis(void) {
    sval_vs_t s;
    long      h, v;
    sval_vs_start(&s);
    sval_vs_add_relative(&s, 0.9f, 21.0f, -1.0f); // sideways drift inside the dead zone
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -2400, 2));
    sval_vs_add_relative(&s, -12.9f, -42.0f, -1.0f); // now 12 mm left, 21 mm up: both axes
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(NEAR(h, -1320, 2) && NEAR(v, 2400, 2));
}

static void test_axis_lock(void) {
    sval_vs_t s;
    long      h, v;
    sval_vs_start(&s);
    sval_vs_add_relative(&s, 3.0f, 6.0f, -1.0f); // vertical leaves first
    run(&s, true, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -600, 2) && s.lock == 1);
    sval_vs_add_relative(&s, 20.0f, 0.0f, -1.0f); // now mostly sideways: still vertical
    run(&s, true, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -600, 2));
    sval_vs_add_relative(&s, -23.0f, -6.0f, -1.0f); // back in the dead zone: free
    run(&s, true, 100, 8, 120, &h, &v);
    CHECK(s.lock == 0);
    sval_vs_add_relative(&s, 6.0f, 0.0f, -1.0f);
    run(&s, true, 1000, 8, 120, &h, &v);
    CHECK(NEAR(h, 600, 2) && v == 0 && s.lock == 2);
}

static void test_pad(void) {
    sval_vs_t s;
    long      h, v;
    sval_vs_start(&s);
    sval_vs_pad(&s, 1, 7, 20.0f, 20.0f); // finger on the right pad: the anchor
    CHECK(sval_vs_touching(&s) && s.pad == 1);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0);
    sval_vs_pad(&s, 1, 7, 20.0f, 32.0f); // 12 mm down
    sval_vs_pad(&s, 0, 9, 5.0f, 5.0f);   // a finger lands on the left pad: ignored
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -2400, 2) && s.pad == 1);
    sval_vs_pad(&s, 0, 9, 30.0f, 5.0f); // the left finger moves: still ignored, never merged
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && NEAR(v, -2400, 2));
    sval_vs_pad(&s, 1, 0, 0.0f, 0.0f); // lift: stop
    CHECK(!sval_vs_touching(&s));
    sval_vs_pad(&s, 0, 9, 30.0f, 5.0f); // the resting left finger never takes over
    CHECK(!sval_vs_touching(&s));
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0);
    sval_vs_pad(&s, 0, 10, 10.0f, 10.0f); // a finger that lands later: a new anchor where it landed
    CHECK(sval_vs_touching(&s) && s.pad == 0);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(h == 0 && v == 0);
    sval_vs_pad(&s, 0, 10, 22.0f, 10.0f);
    run(&s, false, 1000, 8, 120, &h, &v);
    CHECK(NEAR(h, 2400, 2) && v == 0);
    // A finger already down at key-down is taken where it is.
    sval_vs_start(&s);
    sval_vs_pad(&s, 0, 0, 0.0f, 0.0f);
    sval_vs_pad(&s, 1, 3, 15.0f, 15.0f);
    CHECK(s.pad == 1 && s.anchor_x == 15.0f);
}

// What the motion report shows: the offset in use and the speed the last step sent at.
static void test_report_state(void) {
    sval_vs_t s;
    float     x, y;
    sval_vs_start(&s);
    sval_vs_add_relative(&s, 0.0f, 6.0f, -1.0f);
    sval_vs_offset(&s, &x, &y);
    CHECK(NEAR(x, 0.0f, 1e-6f) && NEAR(y, 6.0f, 1e-6f));
    int16_t h, v;
    sval_vs_step(&s, &ball, &pad, false, 8.0f, 120.0f, &h, &v);
    CHECK(NEAR(s.speed_h, 0.0f, 1e-6f));
    CHECK(NEAR(s.speed_v, -sval_vs_speed(&ball, 6.0f), 1e-4f)); // toward the user scrolls down
    // A finger takes over the offset from its anchor.
    sval_vs_pad(&s, 1, 7, 10.0f, 10.0f);
    sval_vs_pad(&s, 1, 7, 14.0f, 9.0f);
    sval_vs_offset(&s, &x, &y);
    CHECK(NEAR(x, 4.0f, 1e-5f) && NEAR(y, -1.0f, 1e-5f));
    sval_vs_step(&s, &ball, &pad, false, 8.0f, 120.0f, &h, &v);
    CHECK(NEAR(s.speed_h, sval_vs_speed(&pad, 4.0f), 1e-4f));
    CHECK(NEAR(s.speed_v, 0.0f, 1e-6f)); // inside the dead zone
    sval_vs_stop(&s);
    sval_vs_offset(&s, &x, &y);
    CHECK(x == 0.0f && y == 0.0f && s.speed_h == 0.0f && s.speed_v == 0.0f);
}

int main(void) {
    test_curve();
    test_report_state();
    test_ball();
    test_dead_zone_per_axis();
    test_axis_lock();
    test_pad();
    if (failures) return 1;
    puts("velocity scroll: OK");
    return 0;
}
