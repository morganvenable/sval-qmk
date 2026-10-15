// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad (experiment/native-ptp). See tools/multitouch and
// the handoff DESIGN.md. Only built with SVAL_NATIVE_PTP=yes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Who owns the pads. One owner at a time, chosen explicitly (0xFC op 0x42).
enum sval_touch_owner {
    SVAL_TOUCH_OWNER_MOUSE    = 0, // the chip's relative mouse
    SVAL_TOUCH_OWNER_HOST     = 1, // host injection through the 0xFC stream
    SVAL_TOUCH_OWNER_DETECTOR = 2, // firmware gesture detector (reserved)
    SVAL_TOUCH_OWNER_NATIVE   = 3, // native Precision Touchpad (this file)
};

// Tunables. Build-time defaults; azoteq/config.h sets them for real builds.
#ifndef SVAL_PTP_REMOTE_POLL_MS
#    define SVAL_PTP_REMOTE_POLL_MS 5
#endif
#ifndef SVAL_PTP_REMOTE_STALE_MS
#    define SVAL_PTP_REMOTE_STALE_MS 50
#endif
// The other half's pad is fetched on news (its split pointing report carries
// a counter); these bound the gaps when there is none.
#ifndef SVAL_PTP_REMOTE_BACKUP_MS
#    define SVAL_PTP_REMOTE_BACKUP_MS 10 // contacts down or pad not armed
#endif
#ifndef SVAL_PTP_REMOTE_IDLE_MS
#    define SVAL_PTP_REMOTE_IDLE_MS 250 // quiet pad: heartbeat
#endif
#ifndef SVAL_PTP_REMOTE_HINT_LOST_MS
#    define SVAL_PTP_REMOTE_HINT_LOST_MS 300 // no hint this long: poll as before
#endif
#ifndef SVAL_PTP_MIN_FRAME_US
#    define SVAL_PTP_MIN_FRAME_US 4000
#endif
#ifndef SVAL_PTP_KEEPALIVE_MS
#    define SVAL_PTP_KEEPALIVE_MS 50
#endif
#ifndef SVAL_PTP_PALM_AREA
#    define SVAL_PTP_PALM_AREA 0 // 0 = off; else area above this clears Confidence
#endif
#ifndef SVAL_PTP_REMAP_COUNTS
#    define SVAL_PTP_REMAP_COUNTS 400 // continuity across a stream restart
#endif
#ifndef SVAL_PTP_FLIP_L_X
#    define SVAL_PTP_FLIP_L_X 0
#endif
#ifndef SVAL_PTP_FLIP_L_Y
#    define SVAL_PTP_FLIP_L_Y 0
#endif
#ifndef SVAL_PTP_FLIP_R_X
#    define SVAL_PTP_FLIP_R_X 0
#endif
#ifndef SVAL_PTP_FLIP_R_Y
#    define SVAL_PTP_FLIP_R_Y 0
#endif
#ifndef SVAL_PTP_KEY_BUTTONS
#    define SVAL_PTP_KEY_BUTTONS 0
#endif
#ifndef SVAL_PTP_MOUSE_BEFORE_MODE3
#    define SVAL_PTP_MOUSE_BEFORE_MODE3 1
#endif

#define SVAL_PTP_INPUT_SIZE 35
#define SVAL_PTP_MAX_CONTACTS 5
#define SVAL_PTP_CERT_SIZE 257

// --- owner and mouse coordination (multitouch.c, azoteq.c, svalboard.c) ---
void sval_ptp_set_native(bool native);
bool sval_ptp_wants_frames(void);  // feed pad frames to sval_ptp_feed()
bool sval_ptp_active(void);        // NATIVE and Input Mode 3: PTP owns the pads
bool sval_ptp_suppress_mouse(void);// the pads' mouse output must be zero
uint16_t sval_ptp_take_motion(uint8_t pad); // admitted contact motion, counts
bool sval_ptp_key_button(uint8_t button, bool pressed); // SVAL_PTP_KEY_BUTTONS

// --- contacts (multitouch.c) ---
// One version-1 56-byte pad frame: pad, flags, mask, seq, size, time, epoch,
// five 8-byte slots (lifetime id, x, y, strength, area).
void sval_ptp_feed(const uint8_t *frame);
void sval_ptp_pad_lost(uint8_t pad); // remote snapshot went stale
bool sval_ptp_pad_quiet(uint8_t pad); // all up, armed, nothing pending
void sval_ptp_task(void);

// --- status for the owner command (16 bytes) ---
void sval_ptp_status(uint8_t *out);

// --- feature reports (called from tmk_core, see usb_ptp.h) ---
// report_type: 1 input, 3 feature. Returns the length, or 0 to stall.
uint16_t sval_ptp_get_report(uint8_t report_type, uint8_t report_id, const uint8_t **data);
// True when stored; false stalls.
bool sval_ptp_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data, uint16_t length);
bool sval_ptp_set_report_allowed(uint8_t report_type, uint8_t report_id, uint16_t length);
void sval_ptp_usb_reset(void);
