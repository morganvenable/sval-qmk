// Pure helpers for the pointing fragments (Sval 0x18) and POINTING_MOTION (Sval 0x2D),
// kept free of QMK headers so tests/sval_pointing can build them on the host.
// Layout of the 0x2D reply: docs/protocol.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SVAL_MOTION_FORMAT     1
#define SVAL_MOTION_REPORT_LEN 24 // bytes after [0xDF] [0x2D]
#define SVAL_MOTION_HAND_LEN   10
#define SVAL_MOTION_LEFT_OFF   3
#define SVAL_MOTION_RIGHT_OFF  13
#define SVAL_MOTION_AXIS_OFF   23

// Extension after the 24-byte report (sval_flag2_pointing_motion_readers), sent only
// when the packet has room (64-byte reports): the reader id the request named and
// each hand's sensor CPI (0 when the hand has no CPI-based sensor).
#define SVAL_MOTION_READER_OFF    24
#define SVAL_MOTION_LEFT_CPI_OFF  25 // uint16 little-endian
#define SVAL_MOTION_RIGHT_CPI_OFF 27 // uint16 little-endian
#define SVAL_MOTION_EXT_LEN       29 // bytes after [0xDF] [0x2D] with the extension

// Readers that keep their own counts. Reader 0 is a client that names none (older
// clients send zero padding there); it gets a slot like any other.
#ifndef SVAL_MOTION_READERS
#    define SVAL_MOTION_READERS 4
#endif

// Global mode bits (reply byte 1).
#define SVAL_MOTION_SNIPER         0x01 // sniper / precision active
#define SVAL_MOTION_BOOST          0x02 // boost active
#define SVAL_MOTION_AUTOMOUSE_ON   0x04 // the auto-mouse layer is on
#define SVAL_MOTION_AXIS_LOCK      0x08 // axis scroll lock is in effect
#define SVAL_MOTION_NATURAL_SCROLL 0x10
#define SVAL_MOTION_SCROLL_KEY     0x20 // scroll hold or scroll toggle key active

// Per-hand flags (block byte 0).
#define SVAL_MOTION_VALID         0x01
#define SVAL_MOTION_SCROLLING     0x02
#define SVAL_MOTION_SATURATED     0x04
#define SVAL_MOTION_AUTOMOUSE     0x08 // auto-mouse is enabled for this hand
#define SVAL_MOTION_TOUCH_VALID   0x10
#define SVAL_MOTION_FINGERS_SHIFT 5 // bits 5-6, 0..3

// One hand's motion since the last 0x2D read.
typedef struct {
    int32_t x, y;
    int16_t h, v;
    uint8_t buttons;
    bool    saturated;
} sval_motion_acc_t;

static inline int32_t sval_motion_add32(int32_t a, int32_t b, bool *sat) {
    if (b > 0 && a > INT32_MAX - b) { *sat = true; return INT32_MAX; }
    if (b < 0 && a < INT32_MIN - b) { *sat = true; return INT32_MIN; }
    return a + b;
}

static inline int32_t sval_motion_clamp(int32_t v, int32_t lo, int32_t hi, bool *sat) {
    if (v > hi) { *sat = true; return hi; }
    if (v < lo) { *sat = true; return lo; }
    return v;
}

static inline void sval_motion_accumulate(sval_motion_acc_t *acc, int32_t x, int32_t y, int32_t h, int32_t v, uint8_t buttons) {
    acc->x = sval_motion_add32(acc->x, x, &acc->saturated);
    acc->y = sval_motion_add32(acc->y, y, &acc->saturated);
    acc->h = (int16_t)sval_motion_clamp((int32_t)acc->h + h, INT16_MIN, INT16_MAX, &acc->saturated);
    acc->v = (int16_t)sval_motion_clamp((int32_t)acc->v + v, INT16_MIN, INT16_MAX, &acc->saturated);
    acc->buttons |= buttons;
}

// One reader's counts for both hands ([0] left) and when it last read them.
typedef struct {
    bool              used;
    uint8_t           id;
    uint32_t          last_read;
    sval_motion_acc_t acc[2];
} sval_motion_reader_t;

// A reader is live (its counts still grow) while it has read within active_ms.
static inline bool sval_motion_reader_live(const sval_motion_reader_t *r, uint32_t now, uint32_t active_ms) {
    return r->used && (uint32_t)(now - r->last_read) < active_ms;
}

static inline bool sval_motion_any_live(const sval_motion_reader_t *t, uint8_t n, uint32_t now, uint32_t active_ms) {
    for (uint8_t i = 0; i < n; i++)
        if (sval_motion_reader_live(&t[i], now, active_ms)) return true;
    return false;
}

// Adds one hand's report to every live reader, so each one sees the full motion
// however often it reads.
static inline void sval_motion_readers_accumulate(sval_motion_reader_t *t, uint8_t n, uint32_t now, uint32_t active_ms, uint8_t hand, int32_t x, int32_t y, int32_t h, int32_t v, uint8_t buttons) {
    for (uint8_t i = 0; i < n; i++)
        if (sval_motion_reader_live(&t[i], now, active_ms)) sval_motion_accumulate(&t[i].acc[hand], x, y, h, v, buttons);
}

// The slot for reader `id`: its own while it has one, else a free slot, else the one
// read longest ago (an expired reader first, since all expired ones are older than
// any live one). *first is true when the slot is new to this reader: its counts
// start at zero and the reply's elapsed byte is 0.
static inline sval_motion_reader_t *sval_motion_reader_claim(sval_motion_reader_t *t, uint8_t n, uint8_t id, uint32_t now, bool *first) {
    sval_motion_reader_t *pick = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (t[i].used && t[i].id == id) {
            *first = false;
            return &t[i];
        }
    }
    for (uint8_t i = 0; i < n && !pick; i++)
        if (!t[i].used) pick = &t[i];
    if (!pick) {
        pick = &t[0];
        for (uint8_t i = 1; i < n; i++)
            if ((uint32_t)(now - t[i].last_read) > (uint32_t)(now - pick->last_read)) pick = &t[i];
    }
    for (uint8_t k = 0; k < 2; k++) pick->acc[k] = (sval_motion_acc_t){0};
    pick->used      = true;
    pick->id        = id;
    pick->last_read = now;
    *first          = true;
    return pick;
}

static inline void sval_motion_put16(uint8_t *out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)(v >> 8);
}

// Writes one 10-byte hand block. flags carries VALID/SCROLLING/AUTOMOUSE/touch bits;
// SATURATED is added when anything was clamped. The caller clears acc afterwards.
static inline void sval_motion_pack_hand(uint8_t *out, const sval_motion_acc_t *acc, uint8_t flags) {
    bool    sat = acc->saturated;
    int16_t dx  = (int16_t)sval_motion_clamp(acc->x, INT16_MIN, INT16_MAX, &sat);
    int16_t dy  = (int16_t)sval_motion_clamp(acc->y, INT16_MIN, INT16_MAX, &sat);
    int8_t  h   = (int8_t)sval_motion_clamp(acc->h, INT8_MIN, INT8_MAX, &sat);
    int8_t  v   = (int8_t)sval_motion_clamp(acc->v, INT8_MIN, INT8_MAX, &sat);
    if (sat) flags |= SVAL_MOTION_SATURATED;
    out[0] = flags & 0x7F;
    out[1] = (uint8_t)((uint16_t)dx & 0xFF);
    out[2] = (uint8_t)((uint16_t)dx >> 8);
    out[3] = (uint8_t)((uint16_t)dy & 0xFF);
    out[4] = (uint8_t)((uint16_t)dy >> 8);
    out[5] = (uint8_t)h;
    out[6] = (uint8_t)v;
    out[7] = acc->buttons;
    out[8] = 0; // touch x: positions are not reported yet
    out[9] = 0; // touch y
}

// Elapsed ms for reply byte 2: 0 on the first read, else saturating at 255.
static inline uint8_t sval_motion_elapsed(bool first_read, uint32_t elapsed_ms) {
    if (first_read) return 0;
    return elapsed_ms > 255 ? 255 : (uint8_t)elapsed_ms;
}

// Starts reader `id`'s read at `now`: claims its slot, sets *elapsed for reply byte
// 2 and restarts its clock. The caller packs the slot's counts, then clears them.
static inline sval_motion_reader_t *sval_motion_reader_begin_read(sval_motion_reader_t *t, uint8_t n, uint8_t id, uint32_t now, uint8_t *elapsed) {
    bool                  first;
    sval_motion_reader_t *r = sval_motion_reader_claim(t, n, id, now, &first);
    *elapsed                = sval_motion_elapsed(first, now - r->last_read);
    r->last_read            = now;
    return r;
}

// The pointing fragment for a POINTING_HARDWARE `detected` code (enum sval_pointer_sensor),
// or 0xFF when nothing is known yet. Needs the generated SVAL_FRAGMENT_ID_POINTING_* names.
#ifdef SVAL_FRAGMENT_ID_POINTING_NONE
static inline uint8_t sval_pointing_fragment_for_detected(uint8_t detected) {
    switch (detected) {
        case 1: return SVAL_FRAGMENT_ID_POINTING_TRACKBALL_PMW3360; // SVAL_SENSOR_PMW3360
        case 2: return SVAL_FRAGMENT_ID_POINTING_TRACKBALL_PMW3389; // SVAL_SENSOR_PMW3389
        case 3: return SVAL_FRAGMENT_ID_POINTING_NONE;              // SVAL_SENSOR_NO_RESPONSE
        case 4: return SVAL_FRAGMENT_ID_POINTING_UNKNOWN;           // SVAL_SENSOR_UNKNOWN
        case 5: return SVAL_FRAGMENT_ID_POINTING_TOUCHPAD_AZOTEQ;   // SVAL_SENSOR_AZOTEQ
        case 6: return SVAL_FRAGMENT_ID_POINTING_TRACKPOINT;        // SVAL_SENSOR_TRACKPOINT
        default: return 0xFF;                                       // 0: not checked yet
    }
}
#endif
