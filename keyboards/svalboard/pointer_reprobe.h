// When a half keeps looking for its pointing module (pointer_reprobe.c).
//
// A half whose module didn't answer at boot looks again at runtime: a module that powered up
// late, or a cable plugged in after boot, starts without a replug. The first search runs
// 1.5 s after boot, late enough for a TrackPoint's power-on self-test. Then the searches back
// off: 3 s, 6 s and 12 s after boot catch a module that was slow to come up or a cable seated
// during the first seconds; after that, every 30 s, for as long as nothing is found. One
// search of an auto build with nothing attached busy-waits about 11 ms, one bus per main
// loop pass: PS/2 ~10.1 ms (the device's 10 ms window to start clocking), SPI ~0.5 ms, the
// trackpad's I2C check well under 1 ms (tests/sval_pointing/sim.c). So the steady rate
// spends under 0.04% of the time, the longest stall is the one PS/2 pass, and a module
// plugged in later waits half a minute at most.
//
// A half stops looking for good once its module works, once its own bus answers with a
// module this firmware can't drive (a per-module build: the host names the right .uf2 from
// Sval 0x2C), or once a module answered and failed to start SVAL_REPROBE_INIT_TRIES times
// (a trackball's start uploads its firmware, ~150 ms each time). Plain C with no QMK
// headers, for the host test (tests/sval_pointing/reprobe_test.c); include it after the
// enum sval_pointer_sensor (svalboard.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SVAL_REPROBE_FIRST_MS 1500u
#define SVAL_REPROBE_STEADY_MS 30000u
#define SVAL_REPROBE_INIT_TRIES 4u

// The wait before search number attempt (0 is the first): from boot for the first, from the
// start of the previous search for the rest.
static inline uint32_t sval_reprobe_delay_ms(uint16_t attempt) {
    static const uint16_t early[] = {1500, 1500, 3000, 6000}; // 1.5, 3, 6 and 12 s after boot
    return attempt < sizeof(early) / sizeof(early[0]) ? early[attempt] : SVAL_REPROBE_STEADY_MS;
}

// Whether a half still looks for its module, from its own identity (sval_pointer_identity:
// enum sval_pointer_sensor codes), what the last search of the other buses found
// (no_response: nothing) and how many found modules failed to start.
//   built_for, detected  the half's own module: what the build drives, and what answered
//   init_ok              SVAL_POINTER_INIT_OK: its driver is running
//   auto_build           SVAL_POINTER_AUTO: the build starts whichever module the search finds
static inline bool sval_reprobe_wanted(uint8_t built_for, uint8_t detected, bool init_ok, bool auto_build, uint8_t found, uint8_t init_failures) {
    if (init_ok) return false;                                  // working: never searched again
    if (init_failures >= SVAL_REPROBE_INIT_TRIES) return false; // answers, but won't start
    // Its own bus answered with a module the build can't drive.
    if (built_for != SVAL_SENSOR_NONE && detected != SVAL_SENSOR_NONE && detected != SVAL_SENSOR_NO_RESPONSE && detected != built_for) return false;
    // Another bus has a module, and the build can't start it.
    if (!auto_build && found != SVAL_SENSOR_NONE && found != SVAL_SENSOR_NO_RESPONSE) return false;
    return true;
}
