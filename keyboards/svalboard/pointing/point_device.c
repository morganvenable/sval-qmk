// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_device.h"

// The sensor numbers of enum sval_pointer_sensor (svalboard.h), kept here so the host
// tests need no board headers.
enum { SENSOR_PMW3360 = 1, SENSOR_PMW3389 = 2, SENSOR_AZOTEQ = 5, SENSOR_TRACKPOINT = 6 };

uint8_t sval_point_device(uint8_t sensor) {
    switch (sensor) {
        case SENSOR_PMW3360:
        case SENSOR_PMW3389:
            return SVAL_POINT_DEVICE_BALL;
        case SENSOR_AZOTEQ:
            return SVAL_POINT_DEVICE_PAD;
        case SENSOR_TRACKPOINT:
            return SVAL_POINT_DEVICE_STICK;
        default:
            return SVAL_POINT_DEVICE_NONE;
    }
}

sval_point_scale_t sval_point_scale(uint8_t sensor, uint16_t dpi, bool contacts, uint8_t pad_gain_x16, uint8_t stick_gain_x16) {
    sval_point_scale_t s = {dpi, SVAL_POINT_GAIN_ONE, sval_point_device(sensor), false};
    switch (s.device) {
        case SVAL_POINT_DEVICE_PAD:
            // The chip's own relative output stops at its resolution; contacts are
            // converted to the hand's CPI exactly.
            if (!contacts && s.cpi > SVAL_POINT_PAD_FULL_CPI) s.cpi = SVAL_POINT_PAD_FULL_CPI;
            s.contacts = contacts;
            s.gain_x16 = pad_gain_x16;
            break;
        case SVAL_POINT_DEVICE_STICK:
            // CPI changes nothing on a TrackPoint (pointing_device_ps2.c ignores it).
            s.cpi      = 800;
            s.gain_x16 = stick_gain_x16;
            break;
        default:
            // A trackball, or a hand not known yet: CPI only, as before.
            break;
    }
    if (!s.gain_x16) s.gain_x16 = SVAL_POINT_GAIN_ONE;
    return s;
}
