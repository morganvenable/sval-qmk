// Device gains (pointing/point_device.c): each device type's counts in the tools'
// normalized units (trackball counts at 800 CPI), so a tool feels about the same on any
// module.
#include <assert.h>
#include <stdio.h>
#include "point_device.h"
#include "point_motion.h"

// Normalized counts (whole) for n reports of delta counts at a scale.
static int32_t normalized(sval_point_scale_t s, int16_t delta, int n) {
    int64_t remainder = 0, total = 0;
    for (int i = 0; i < n; ++i)
        total += sval_point_normalize_gain(delta, s.cpi, s.gain_x16, &remainder);
    return (int32_t)(total / SVAL_POINT_ONE);
}

int main(void) {
    // Sensor numbers (svalboard.h): 1 PMW3360, 2 PMW3389, 5 Azoteq, 6 TrackPoint.
    assert(sval_point_device(1) == SVAL_POINT_DEVICE_BALL && sval_point_device(2) == SVAL_POINT_DEVICE_BALL);
    assert(sval_point_device(5) == SVAL_POINT_DEVICE_PAD && sval_point_device(6) == SVAL_POINT_DEVICE_STICK);
    assert(sval_point_device(0) == SVAL_POINT_DEVICE_NONE && sval_point_device(3) == SVAL_POINT_DEVICE_NONE && sval_point_device(4) == SVAL_POINT_DEVICE_NONE);

    // Trackball: CPI is the whole conversion; no device gain, whatever the pad and
    // stick gains are.
    sval_point_scale_t ball = sval_point_scale(2, 1600, false, 48, 8);
    assert(ball.device == SVAL_POINT_DEVICE_BALL && ball.cpi == 1600 && ball.gain_x16 == SVAL_POINT_GAIN_ONE && !ball.contacts);
    assert(normalized(ball, 128, 1) == 64);
    assert(normalized(sval_point_scale(1, 400, false, 48, 8), 32, 1) == 64);
    // Small reports at a high CPI add up exactly (the remainder carries).
    assert(normalized(sval_point_scale(2, 3200, false, 32, 32), 1, 256) == 64);

    // Touchpad, its own mouse output: CPI, capped at the pad's resolution, times the gain.
    sval_point_scale_t pad = sval_point_scale(5, 800, false, SVAL_POINT_PAD_GAIN_DEFAULT, 8);
    assert(pad.device == SVAL_POINT_DEVICE_PAD && pad.cpi == 800 && pad.gain_x16 == 32);
    assert(normalized(pad, 32, 1) == 64); // 2.0: half the travel of a ball for a step
    assert(SVAL_POINT_PAD_FULL_CPI == 1210);
    sval_point_scale_t pad_fast = sval_point_scale(5, 1600, false, 16, 8);
    assert(pad_fast.cpi == 1210 && normalized(pad_fast, 1210, 1) == 800); // an inch is an inch
    // Contacts are converted to the hand's CPI exactly: no cap, and marked.
    sval_point_scale_t pad_contacts = sval_point_scale(5, 1600, true, 16, 8);
    assert(pad_contacts.cpi == 1600 && pad_contacts.contacts);
    assert(!sval_point_scale_equal(pad_fast, pad_contacts));
    // The touchpad gain is its own setting.
    assert(normalized(sval_point_scale(5, 800, false, 48, 8), 32, 1) == 96);

    // TrackPoint: CPI changes nothing; logical 800 CPI times the gain.
    sval_point_scale_t stick = sval_point_scale(6, 1600, false, 48, SVAL_POINT_STICK_GAIN_DEFAULT);
    assert(stick.device == SVAL_POINT_DEVICE_STICK && stick.cpi == 800 && stick.gain_x16 == 32);
    assert(normalized(stick, 32, 1) == 64);
    assert(normalized(sval_point_scale(6, 200, false, 48, 8), 64, 1) == 32); // 0.5
    assert(!sval_point_scale(6, 800, true, 16, 16).contacts);

    // Gains are per device type, not per hand: the same module gets the same scale on
    // either hand (the scale has no hand in it), and a zero gain never stops a pointer.
    assert(sval_point_scale_equal(sval_point_scale(5, 800, false, 32, 32), sval_point_scale(5, 800, false, 32, 32)));
    assert(sval_point_scale(5, 800, false, 0, 0).gain_x16 == SVAL_POINT_GAIN_ONE);
    assert(sval_point_scale(6, 800, false, 0, 0).gain_x16 == SVAL_POINT_GAIN_ONE);
    // An unknown hand (no module yet) is treated as a trackball at its CPI.
    sval_point_scale_t none = sval_point_scale(0, 800, false, 48, 8);
    assert(none.cpi == 800 && none.gain_x16 == SVAL_POINT_GAIN_ONE);

    // The same step takes the same normalized travel on each device at its default.
    assert(normalized(ball, 320, 1) == normalized(sval_point_scale(5, 800, false, 32, 32), 80, 1));
    assert(normalized(sval_point_scale(6, 800, false, 32, 32), 80, 1) == 160);
    puts("devices: device types, trackball CPI only, touchpad CPI cap and gain, contacts uncapped, TrackPoint logical CPI and gain, per type not per hand passed");
}
