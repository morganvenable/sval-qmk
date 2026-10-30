// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Two halves on separate USB cables, bridged by a host relay (prototype,
// docs/two-cable.md). Built only with SVAL_TWO_CABLE=yes.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "report.h"

// Wrapped raw HID protocol byte: [0xDD][client:4][0xFB][op] ...
#define SVAL_TWO_CABLE_PROTO 0xFB

#ifdef SVAL_TWO_CABLE
// A wrapped 0xFB packet (client_wrapper.c). Replies in place when the op
// answers; returns true when the buffer holds a reply to send.
bool sval_two_cable_command(uint8_t *data, uint8_t length);
void sval_two_cable_task(void);
// This half runs the keymap for both halves; the other half's rows come
// from the relay.
bool sval_two_cable_lead_live(void);
// This half only reports its rows and motion to the relay; it types nothing.
bool sval_two_cable_follower_live(void);
// Pointing: the follower takes its own hand's report; the lead fills in the
// other hand's (svalboard.c, pointing_device_task_combined_kb).
void sval_two_cable_pointing(report_mouse_t *left, report_mouse_t *right);
#else
static inline bool sval_two_cable_lead_live(void) {
    return false;
}
static inline bool sval_two_cable_follower_live(void) {
    return false;
}
#endif
