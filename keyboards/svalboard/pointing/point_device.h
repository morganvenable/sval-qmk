// SPDX-License-Identifier: GPL-2.0-or-later
// How the pointing tools read each kind of pointing module (docs/pointing-tools.md,
// "Devices"). The tools measure motion in normalized counts: counts of trackball travel
// at 800 CPI. Each device type has one device gain that turns its raw counts into those
// units, so a tool's speed means about the same on any module and is tuned once.
//
// - Trackball (PMW3360, PMW3389): its counts are a calibrated distance at the hand's CPI,
//   so the CPI is the whole conversion. It has no device gain.
// - Touchpad (Azoteq TPS43): its counts are a distance too (at the hand's CPI, capped at
//   the pad's own resolution), times the touchpad gain, since a finger has 43 mm of travel
//   where a ball rolls on.
// - TrackPoint: its counts measure force, not distance (rate control), so they are taken
//   as a logical 800 CPI times the TrackPoint gain.
//
// Gains are per device type, not per hand: a module moved to the other half keeps its
// feel. Free of QMK headers so tests/sval_pointing builds it on the host.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    SVAL_POINT_DEVICE_NONE  = 0,
    SVAL_POINT_DEVICE_BALL  = 1,
    SVAL_POINT_DEVICE_PAD   = 2,
    SVAL_POINT_DEVICE_STICK = 3,
};
// Device gains are x16: 16 is 1.0, 1 is 1/16, 255 almost 16. 0 is never valid.
#define SVAL_POINT_GAIN_ONE 16
// Defaults, with the reasons in docs/pointing-tools.md.
#define SVAL_POINT_PAD_GAIN_DEFAULT 32   // 2.0
#define SVAL_POINT_STICK_GAIN_DEFAULT 32 // 2.0

// Counts per inch the TPS43 resolves across its 43 mm: 2048 counts (azoteq/azoteq.c).
// A touchpad at a higher CPI still reports at this resolution.
#define SVAL_POINT_PAD_FULL_CPI ((2048u * 254u + 215u) / 430u) // 1210

typedef struct {
    uint16_t cpi;      // the counts per inch of the counts as they arrive (logical for a TrackPoint)
    uint8_t  gain_x16; // the device gain (SVAL_POINT_GAIN_ONE for a trackball)
    uint8_t  device;   // SVAL_POINT_DEVICE_*
    bool     contacts; // a touchpad read from its contacts (sval_point_contact reports lifts)
} sval_point_scale_t;

// The device type of a module (enum sval_pointer_sensor in svalboard.h).
uint8_t sval_point_device(uint8_t sensor);
// How to normalize a hand's counts. dpi is the hand's CPI setting. contacts: a touchpad
// whose motion comes from its contacts, already converted to dpi counts (no resolution cap).
sval_point_scale_t sval_point_scale(uint8_t sensor, uint16_t dpi, bool contacts, uint8_t pad_gain_x16, uint8_t stick_gain_x16);
static inline bool sval_point_scale_equal(sval_point_scale_t a, sval_point_scale_t b) {
    return a.cpi == b.cpi && a.gain_x16 == b.gain_x16 && a.device == b.device && a.contacts == b.contacts;
}
