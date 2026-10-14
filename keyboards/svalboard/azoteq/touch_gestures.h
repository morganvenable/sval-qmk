// SPDX-License-Identifier: GPL-2.0-or-later
// Firmware-owned touch gestures for the unified Azoteq pair (experimental).
// See keyboards/svalboard/docs/protocol.md "Touch gestures" and
// tools/multitouch/README.md. Everything here runs on the USB half only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TG_PAYLOAD_VERSION 1
#define TG_PAYLOAD_SIZE 21
#define TG_BLOCK_SIZE (1 + TG_PAYLOAD_SIZE) // stamp + payload v1
#define TG_STAMP 0xA1                       // "touch gestures, v1"

// Payload byte 0: owners and options.
#define TG_OWN_SCROLL_CHIP 0x01 // two-finger scroll: 0 Windows, 1 chip
#define TG_OWN_SWIPE_FW 0x02    // multi-finger swipe: 0 Windows, 1 firmware
#define TG_OWN_PINCH_FW 0x04    // pinch: 0 Windows, 1 firmware
#define TG_OPT_SPAN 0x08        // firmware gestures may span both pads
#define TG_OPT_HOST_MERGE 0x10  // host always merges cross-pad contacts

// Compile-time tunables (hack here).
#ifndef TG_DROP_GRACE_MS
#    define TG_DROP_GRACE_MS 30 // a contact that drops out is held this long
#endif
#ifndef TG_PINCH_HOLD_MS
#    define TG_PINCH_HOLD_MS 60 // chip wheel held after a second finger lands
#endif
#ifndef TG_STALE_MS
#    define TG_STALE_MS 100 // a pad with no frame for this long reads as empty
#endif
// Per-pad flips, matching unified.py's defaults (all off).
#ifndef TG_FLIP_LEFT_X
#    define TG_FLIP_LEFT_X 0
#endif
#ifndef TG_FLIP_LEFT_Y
#    define TG_FLIP_LEFT_Y 0
#endif
#ifndef TG_FLIP_RIGHT_X
#    define TG_FLIP_RIGHT_X 0
#endif
#ifndef TG_FLIP_RIGHT_Y
#    define TG_FLIP_RIGHT_Y 0
#endif

enum tg_output { TG_SWIPE_LEFT, TG_SWIPE_RIGHT, TG_SWIPE_UP, TG_SWIPE_DOWN, TG_PINCH_OUT, TG_PINCH_IN, TG_OUTPUTS };

typedef struct {
    uint8_t  owners;            // TG_OWN_* / TG_OPT_*
    uint8_t  swipe_fingers;     // 3-5
    uint8_t  sustain_min;       // 1-5, <= swipe_fingers
    uint8_t  swipe_mm;          // 4-40
    uint8_t  swipe_ratio_x10;   // 10-50
    uint8_t  settle_ms;         // 0-200
    uint8_t  swipe_window_10ms; // 10-200
    uint8_t  swipe_repeat_mm;   // 0 = once, else 4-40
    uint8_t  pinch_start_x2;    // half mm, 2-40
    uint8_t  pinch_step_x2;     // half mm, 1-40
    uint16_t keycodes[TG_OUTPUTS];
} tg_config_t;

extern tg_config_t tg_config;

// Config. Status codes: 0 ok, 1 bad version/length, 2 out of range, 3 no support.
void    tg_defaults(tg_config_t *c);
uint8_t tg_validate(const tg_config_t *c);
void    tg_encode(const tg_config_t *c, uint8_t out[TG_PAYLOAD_SIZE]);
void    tg_decode(const uint8_t in[TG_PAYLOAD_SIZE], tg_config_t *c);
void    tg_load(void);                            // from the storage hooks, defaults on a bad stamp
void    tg_command(uint8_t *data, uint8_t length); // Sval 0x2E, data after "DF 2E"

// Recognizer. pad 0 = left hand, 1 = right hand. frame = the 56-byte 0xFC frame.
void     tg_reset(void);
void     tg_feed(uint8_t pad, const uint8_t *frame, uint32_t now);
void     tg_tick(uint32_t now);
bool     tg_running(void);        // a gesture owner is firmware
bool     tg_claimed(void);        // a firmware gesture owns the pads until all-up
bool     tg_pinch_active(void);   // a firmware pinch is in progress
bool     tg_pinch_hold(uint8_t pad, uint32_t now); // hold this pad's chip wheel
uint8_t  tg_last_gesture(void);   // 0 none, 1 + enum tg_output
uint16_t tg_take_count(void);     // outputs fired, read-and-clear
uint16_t tg_take_claims(void);    // claims made, read-and-clear

// Wheel outputs, picked up by the pointing path (svalboard.c).
void tg_wheel_take(int16_t *h, int16_t *v);
void tg_task(void);

// Platform hooks: firmware versions live in touch_gestures.c; host tests define their own.
void    tg_platform_emit(uint16_t keycode);
void    tg_storage_read(uint8_t block[TG_BLOCK_SIZE]);
void    tg_storage_write(const uint8_t block[TG_BLOCK_SIZE]);
uint8_t sval_mt_lease_mode(void);       // multitouch.c
uint8_t sval_mt_pad_contacts(uint8_t pad); // multitouch.c, 0xFF unknown
