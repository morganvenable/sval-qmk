// SPDX-License-Identifier: GPL-2.0-or-later
// Firmware-owned three-finger swipe and pinch for the unified Azoteq pair.
//
// Input: the same 56-byte contact frames the USB half streams to the host
// (multitouch.c), from both pads. Output: one QMK keycode per recognized
// gesture step. A recognized gesture "claims" the pads until every finger on
// both pads is up; multitouch.c turns the claim into frame flag 0x40 for the
// host (unified mode) or into chip-output suppression (normal mode).
//
// Units: 0.01 mm on one 86 x 40 mm surface (left pad 0-43 mm, right 43-86 mm).
// Hack freely; tests/sval_touch_gestures runs this file on the host.
#ifdef SVAL_MT_HOST_TEST
#    include <stdbool.h>
#    include <stdint.h>
#else
#    include "quantum.h"
#    include "sval.h"
#    ifdef POINTING_DEVICE_ENABLE
#        include "pointing_device.h"
#    endif
#endif
#include "touch_gestures.h"
#include <stdlib.h>
#include <string.h>

tg_config_t tg_config;

// ---------------------------------------------------------------- config

void tg_defaults(tg_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->owners            = TG_OPT_SPAN; // every owner Windows; gestures may span pads
    c->swipe_fingers     = 3;
    c->sustain_min       = 1;
    c->swipe_mm          = 12;
    c->swipe_ratio_x10   = 20;
    c->settle_ms         = 40;
    c->swipe_window_10ms = 60;
    c->swipe_repeat_mm   = 0;
    c->pinch_start_x2    = 8;
    c->pinch_step_x2     = 6;
    c->keycodes[TG_SWIPE_LEFT]  = 0x094F; // Win+Ctrl+Right: next desktop
    c->keycodes[TG_SWIPE_RIGHT] = 0x0950; // Win+Ctrl+Left: previous desktop
    c->keycodes[TG_SWIPE_UP]    = 0x082B; // Win+Tab: Task View
    c->keycodes[TG_SWIPE_DOWN]  = 0x0807; // Win+D: show desktop
    c->keycodes[TG_PINCH_OUT]   = 0x01D9; // Ctrl+wheel up: zoom in
    c->keycodes[TG_PINCH_IN]    = 0x01DA; // Ctrl+wheel down: zoom out
}

uint8_t tg_validate(const tg_config_t *c) {
    if (c->owners & 0xE0) return 2;
    if (c->swipe_fingers < 3 || c->swipe_fingers > 5) return 2;
    if (c->sustain_min < 1 || c->sustain_min > c->swipe_fingers) return 2;
    if (c->swipe_mm < 4 || c->swipe_mm > 40) return 2;
    if (c->swipe_ratio_x10 < 10 || c->swipe_ratio_x10 > 50) return 2;
    if (c->settle_ms > 200) return 2;
    if (c->swipe_window_10ms < 10 || c->swipe_window_10ms > 200) return 2;
    if (c->swipe_repeat_mm && (c->swipe_repeat_mm < 4 || c->swipe_repeat_mm > 40)) return 2;
    if (c->pinch_start_x2 < 2 || c->pinch_start_x2 > 40) return 2;
    if (c->pinch_step_x2 < 1 || c->pinch_step_x2 > 40) return 2;
    return 0;
}

void tg_encode(const tg_config_t *c, uint8_t out[TG_PAYLOAD_SIZE]) {
    out[0] = c->owners;
    out[1] = (c->swipe_fingers & 0x0F) | (uint8_t)(c->sustain_min << 4);
    out[2] = c->swipe_mm;
    out[3] = c->swipe_ratio_x10;
    out[4] = c->settle_ms;
    out[5] = c->swipe_window_10ms;
    out[6] = c->swipe_repeat_mm;
    out[7] = c->pinch_start_x2;
    out[8] = c->pinch_step_x2;
    for (uint8_t i = 0; i < TG_OUTPUTS; i++) {
        out[9 + 2 * i]  = c->keycodes[i] & 0xFF;
        out[10 + 2 * i] = c->keycodes[i] >> 8;
    }
}

void tg_decode(const uint8_t in[TG_PAYLOAD_SIZE], tg_config_t *c) {
    c->owners            = in[0];
    c->swipe_fingers     = in[1] & 0x0F;
    c->sustain_min       = in[1] >> 4;
    c->swipe_mm          = in[2];
    c->swipe_ratio_x10   = in[3];
    c->settle_ms         = in[4];
    c->swipe_window_10ms = in[5];
    c->swipe_repeat_mm   = in[6];
    c->pinch_start_x2    = in[7];
    c->pinch_step_x2     = in[8];
    for (uint8_t i = 0; i < TG_OUTPUTS; i++) c->keycodes[i] = in[9 + 2 * i] | (uint16_t)in[10 + 2 * i] << 8;
}

void tg_load(void) {
    uint8_t block[TG_BLOCK_SIZE];
    tg_storage_read(block);
    tg_config_t c;
    if (block[0] == TG_STAMP) {
        tg_decode(block + 1, &c);
        if (!tg_validate(&c)) {
            tg_config = c;
            return;
        }
    }
    tg_defaults(&tg_config); // a bad stamp or a bad value loads the defaults (RAM only)
}

// ---------------------------------------------------------------- recognizer state

typedef struct {
    uint8_t  id;
    bool     live;   // in the latest frame
    bool     graced; // dropped out, held at its last position for TG_DROP_GRACE_MS
    bool     moved;  // went more than TG_REST_MOVE from where it landed
    bool     rest;   // a resting finger or thumb: never part of a gesture (decided while idle)
    int16_t  x, y, lx, ly; // now, and where it landed
    uint32_t seen, landed;
} tg_contact_t;

enum { ST_IDLE, ST_SWIPE_ARMED, ST_SWIPE, ST_PINCH, ST_WAIT_UP };

static tg_contact_t pads[2][5];
static uint32_t     pad_at[2];        // last frame per pad
static uint32_t     pad_two_since[2]; // when the pad last went from <2 to >=2 contacts
static uint8_t      pad_prev_n[2];
static bool         pad_bad[2];       // the pad's frames are invalid reads or resets
static uint32_t     pad_bad_since[2];

static uint8_t  state;
static bool     claimed;
static int8_t   gpad = -1; // the pad a one-pad gesture is locked to
static bool     arm_pending, pinch_cand;
static uint32_t arm_since, armed_at;
static int32_t  tx, ty, progress, pinch_base, pinch_tx, pinch_ty;
static uint8_t  dir;
static uint8_t  last_gesture;
static uint16_t fired, claims;

// The previous considered contact set, for rebasing when it changes.
static uint16_t prev_sig[10];
static uint8_t  prev_n;
static bool     prev_valid;
static int32_t  prev_cx, prev_cy;

typedef struct {
    uint8_t  n;
    uint16_t sig[10];
    int32_t  x[10], y[10];
} cset_t;

bool tg_running(void) { return (tg_config.owners & (TG_OWN_SWIPE_FW | TG_OWN_PINCH_FW)) != 0; }
bool tg_claimed(void) { return claimed; }
bool tg_pinch_active(void) { return state == ST_PINCH; }
uint8_t tg_last_gesture(void) { return last_gesture; }
uint16_t tg_take_count(void) { uint16_t n = fired; fired = 0; return n; }
uint16_t tg_take_claims(void) { uint16_t n = claims; claims = 0; return n; }

void tg_reset(void) {
    memset(pads, 0, sizeof(pads));
    memset(pad_at, 0, sizeof(pad_at));
    memset(pad_prev_n, 0, sizeof(pad_prev_n));
    memset(pad_bad, 0, sizeof(pad_bad));
    state = ST_IDLE; claimed = false; gpad = -1;
    arm_pending = pinch_cand = prev_valid = false;
    tx = ty = progress = 0;
}

static uint8_t pad_n(uint8_t p) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < 5; i++) n += pads[p][i].live || pads[p][i].graced;
    return n;
}

static int32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int32_t)r;
}

static int8_t considered_pad(void) {
    if (tg_config.owners & TG_OPT_SPAN) return -1;
    if (gpad >= 0) return gpad;
    return pad_n(1) > pad_n(0) ? 1 : 0;
}

static void build(cset_t *s) {
    s->n = 0;
    int8_t only = considered_pad();
    if (state == ST_IDLE) {
        // A finger or thumb that sat still since well before the newest one
        // landed is resting, not gesturing: a thumb on one pad plus a two-finger
        // scroll is a scroll, not a three-finger swipe. Decided only while idle,
        // so a recognized gesture keeps its contacts.
        uint32_t newest = 0;
        bool     any    = false;
        for (uint8_t p = 0; p < 2; p++) {
            if (only >= 0 && p != only) continue;
            for (uint8_t i = 0; i < 5; i++) {
                const tg_contact_t *c = &pads[p][i];
                if (!c->live && !c->graced) continue;
                if (!any || (int32_t)(c->landed - newest) > 0) newest = c->landed;
                any = true;
            }
        }
        for (uint8_t p = 0; p < 2; p++) {
            for (uint8_t i = 0; i < 5; i++) {
                tg_contact_t *c = &pads[p][i];
                c->rest = (c->live || c->graced) && !c->moved && (int32_t)(newest - c->landed) > TG_REST_MS;
            }
        }
    }
    for (uint8_t p = 0; p < 2; p++) {
        if (only >= 0 && p != only) continue;
        for (uint8_t i = 0; i < 5; i++) {
            const tg_contact_t *c = &pads[p][i];
            if (!c->live && !c->graced) continue;
            if (c->rest) continue;
            s->sig[s->n] = (uint16_t)(p << 8 | c->id);
            s->x[s->n] = c->x; s->y[s->n] = c->y;
            s->n++;
        }
    }
}

static void fire(uint8_t output) {
    last_gesture = output + 1;
    fired++;
    uint16_t kc = tg_config.keycodes[output];
    if (kc) tg_platform_emit(kc);
}

static void claim(void) {
    if (!claimed) claims++;
    claimed = true;
}

// Stop anything not yet recognized; the next gesture needs all fingers up.
static void abort_candidate(void) {
    if (state == ST_IDLE || state == ST_SWIPE_ARMED) state = ST_WAIT_UP;
    arm_pending = pinch_cand = false;
}

static void evaluate(uint32_t now) {
    cset_t s;
    build(&s);
    bool    same = prev_valid && s.n == prev_n && !memcmp(s.sig, prev_sig, s.n * sizeof(s.sig[0]));
    int32_t cx = 0, cy = 0, d = 0;
    for (uint8_t i = 0; i < s.n; i++) { cx += s.x[i]; cy += s.y[i]; }
    if (s.n) { cx /= s.n; cy /= s.n; }
    if (s.n == 2) {
        int32_t dx = s.x[1] - s.x[0], dy = s.y[1] - s.y[0];
        d = isqrt32((uint32_t)(dx * dx + dy * dy));
    }
    // Rebase whenever the set of contacts changes: a landing or lift is not travel.
    int32_t dcx = same ? cx - prev_cx : 0, dcy = same ? cy - prev_cy : 0;
    bool    all_up  = !pad_n(0) && !pad_n(1);
    bool    swipe   = tg_config.owners & TG_OWN_SWIPE_FW;
    bool    pinch   = tg_config.owners & TG_OWN_PINCH_FW;
    uint8_t fingers = tg_config.swipe_fingers, sustain = tg_config.sustain_min;
    int8_t  only    = considered_pad();

    switch (state) {
        case ST_IDLE:
            if (swipe && s.n >= fingers) {
                if (!arm_pending) { arm_pending = true; arm_since = now; }
                if (now - arm_since >= tg_config.settle_ms) {
                    state = ST_SWIPE_ARMED; armed_at = now; tx = ty = 0; gpad = only;
                }
            } else arm_pending = false;
            if (state == ST_IDLE && pinch && s.n == 2) {
                if (!pinch_cand || !same) {
                    pinch_cand = true; pinch_base = d; pinch_tx = pinch_ty = 0;
                    if (only >= 0) gpad = only;
                } else {
                    pinch_tx += dcx; pinch_ty += dcy;
                    int32_t dd   = abs(d - pinch_base);
                    int32_t trav = isqrt32((uint32_t)(pinch_tx * pinch_tx + pinch_ty * pinch_ty));
                    if (dd >= tg_config.pinch_start_x2 * 50 && 2 * dd > 3 * trav) {
                        state = ST_PINCH; pinch_base = d; claim();
                    }
                }
            } else if (s.n != 2) pinch_cand = false;
            break;

        case ST_SWIPE_ARMED: {
            if (s.n < sustain) { state = ST_WAIT_UP; break; }
            if (now - armed_at > (uint32_t)tg_config.swipe_window_10ms * 10) { state = ST_WAIT_UP; break; }
            tx += dcx; ty += dcy;
            int32_t ax = abs(tx), ay = abs(ty);
            int32_t major = ax > ay ? ax : ay, minor = ax > ay ? ay : ax;
            if (major >= tg_config.swipe_mm * 100 && major * 10 >= minor * tg_config.swipe_ratio_x10) {
                dir = ax >= ay ? (tx < 0 ? TG_SWIPE_LEFT : TG_SWIPE_RIGHT) : (ty < 0 ? TG_SWIPE_UP : TG_SWIPE_DOWN);
                claim();
                fire(dir);
                state = ST_SWIPE; progress = 0;
            }
            break;
        }

        case ST_SWIPE:
            if (s.n < sustain) { state = ST_WAIT_UP; break; }
            if (tg_config.swipe_repeat_mm) {
                int32_t along = dir == TG_SWIPE_LEFT ? -dcx : dir == TG_SWIPE_RIGHT ? dcx : dir == TG_SWIPE_UP ? -dcy : dcy;
                progress += along;
                if (progress < 0) progress = 0; // reverse travel does nothing in v1
                for (uint8_t k = 0; k < 4 && progress >= tg_config.swipe_repeat_mm * 100; k++) {
                    fire(dir);
                    progress -= tg_config.swipe_repeat_mm * 100;
                }
            }
            break;

        case ST_PINCH:
            // Stays a pinch until all-up; a third finger never turns it into a swipe.
            if (s.n == 2) {
                if (!same) pinch_base = d;
                else {
                    int32_t step = tg_config.pinch_step_x2 * 50;
                    for (uint8_t k = 0; k < 4 && d - pinch_base >= step; k++) { fire(TG_PINCH_OUT); pinch_base += step; }
                    for (uint8_t k = 0; k < 4 && pinch_base - d >= step; k++) { fire(TG_PINCH_IN); pinch_base -= step; }
                }
            }
            break;

        default:
            break;
    }
    if (all_up) {
        state = ST_IDLE; claimed = false; gpad = -1;
        arm_pending = pinch_cand = false;
    } else if (gpad >= 0 && !pad_n(gpad) && state == ST_IDLE) {
        gpad = -1;
    }
    prev_valid = true; prev_n = s.n; prev_cx = cx; prev_cy = cy;
    memcpy(prev_sig, s.sig, s.n * sizeof(s.sig[0]));
}

static void expire(uint32_t now) {
    for (uint8_t p = 0; p < 2; p++) {
        if (now - pad_at[p] > TG_STALE_MS) memset(pads[p], 0, sizeof(pads[p]));
        for (uint8_t i = 0; i < 5; i++) {
            tg_contact_t *c = &pads[p][i];
            if (c->graced && now - c->seen > TG_DROP_GRACE_MS) c->graced = false;
        }
        uint8_t n = pad_n(p);
        if (n >= 2 && pad_prev_n[p] < 2) pad_two_since[p] = now;
        pad_prev_n[p] = n;
    }
}

static void surface_xy(uint8_t pad, uint16_t rx, uint16_t ry, int16_t *x, int16_t *y) {
    int32_t X = (int32_t)rx * 4300 / 2048, Y = (int32_t)ry * 4000 / 1792;
    if (pad ? TG_FLIP_RIGHT_X : TG_FLIP_LEFT_X) X = 4300 - X;
    if (pad ? TG_FLIP_RIGHT_Y : TG_FLIP_LEFT_Y) Y = 4000 - Y;
    *x = (int16_t)(X + pad * 4300);
    *y = (int16_t)Y;
}

void tg_feed(uint8_t pad, const uint8_t *frame, uint32_t now) {
    if (pad > 1 || !tg_running()) return;
    uint8_t flags = frame[2];
    pad_at[pad]   = now;
    if (flags & 0x20) {
        // Split snapshot stale: that pad's contacts are unknown, so they are gone.
        memset(pads[pad], 0, sizeof(pads[pad]));
        abort_candidate();
    } else if (!(flags & 0x03) && (flags & 0x0C) && pad_bad[pad] && now - pad_bad_since[pad] > TG_ABSENT_MS) {
        // Only invalid reads or a sensor stuck in reset for a while: the pad is
        // unplugged or dead. It counts as empty, and the other pad keeps working.
        memset(pads[pad], 0, sizeof(pads[pad]));
    } else if (flags & 0x0F) {
        // Palm, too many fingers, invalid read or sensor reset: keep the last
        // contacts (so all-up still needs a real lift) and drop any candidate.
        if ((flags & 0x0C) && !pad_bad[pad]) { pad_bad[pad] = true; pad_bad_since[pad] = now; }
        abort_candidate();
    } else {
        pad_bad[pad] = false;
        uint8_t mask = frame[3];
        for (uint8_t i = 0; i < 5; i++) {
            tg_contact_t  *c = &pads[pad][i];
            const uint8_t *p = frame + 16 + 8 * i;
            int16_t        x, y;
            surface_xy(pad, p[1] | (uint16_t)p[2] << 8, p[3] | (uint16_t)p[4] << 8, &x, &y);
            if (mask & (1 << i)) {
                if (!c->live && !c->graced) {
                    // A landing adopts one dropped contact on this pad, so a
                    // finger that flickers out and back is not a lift and a
                    // landing: it keeps that contact's landing and rest state.
                    c->landed = now; c->lx = x; c->ly = y; c->moved = c->rest = false;
                    for (uint8_t j = 0; j < 5; j++) {
                        tg_contact_t *d = &pads[pad][j];
                        if (j != i && d->graced) {
                            d->graced = false;
                            c->landed = d->landed; c->lx = d->lx; c->ly = d->ly; c->moved = d->moved; c->rest = d->rest;
                            break;
                        }
                    }
                }
                c->id = p[0];
                c->x = x; c->y = y;
                if (!c->moved && abs(x - c->lx) + abs(y - c->ly) > TG_REST_MOVE) c->moved = true;
                c->live = true; c->graced = false; c->seen = now;
            } else if (c->live) {
                c->live = false; c->graced = true;
            }
        }
    }
    expire(now);
    evaluate(now);
}

void tg_tick(uint32_t now) {
    if (!tg_running()) return;
    expire(now);
    evaluate(now);
}

bool tg_pinch_hold(uint8_t pad, uint32_t now) {
    if (!(tg_config.owners & TG_OWN_PINCH_FW) || pad > 1 || claimed || state != ST_IDLE) return false;
    return pad_n(pad) >= 2 && now - pad_two_since[pad] < TG_PINCH_HOLD_MS;
}

// ---------------------------------------------------------------- Sval 0x2E

static void tg_put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

static void apply(const tg_config_t *c, bool force_save) {
    uint8_t old_block[TG_BLOCK_SIZE], new_block[TG_BLOCK_SIZE];
    old_block[0] = new_block[0] = TG_STAMP;
    tg_encode(&tg_config, old_block + 1);
    tg_encode(c, new_block + 1);
    tg_config = *c;
    if (!tg_running()) tg_reset();
    else if (state != ST_IDLE) state = ST_WAIT_UP; // a live claim is kept until all-up
    if (force_save || memcmp(old_block, new_block, TG_BLOCK_SIZE)) tg_storage_write(new_block);
}

// data[0] = sub-command; the reply is written in place (see protocol.md).
void tg_command(uint8_t *data, uint8_t length) {
    if (length < 2) return;
    uint8_t     sub = data[0];
    tg_config_t c;
    switch (sub) {
        case 0: // GET
            memset(data + 1, 0, length - 1);
            if (length < 4 + TG_PAYLOAD_SIZE) { data[1] = 1; break; }
            data[2] = TG_PAYLOAD_VERSION; data[3] = TG_PAYLOAD_SIZE;
            tg_encode(&tg_config, data + 4);
            break;
        case 1: { // SET
            uint8_t status = 1;
            if (length >= 3 + TG_PAYLOAD_SIZE && data[1] == TG_PAYLOAD_VERSION && data[2] == TG_PAYLOAD_SIZE) {
                tg_decode(data + 3, &c);
                status = tg_validate(&c);
                if (!status) apply(&c, false);
            }
            memset(data + 1, 0, length - 1);
            data[1] = status;
            break;
        }
        case 2: // DEFAULTS
            tg_defaults(&c);
            apply(&c, true);
            memset(data + 1, 0, length - 1);
            if (length >= 2 + TG_PAYLOAD_SIZE) tg_encode(&tg_config, data + 2);
            break;
        case 3: // STATUS
            memset(data + 1, 0, length - 1);
            if (length < 11) { data[1] = 1; break; }
            data[2] = sval_mt_lease_mode();
            data[3] = claimed;
            data[4] = last_gesture;
            tg_put16(data + 5, tg_take_count());
            tg_put16(data + 7, tg_take_claims());
            data[9]  = sval_mt_pad_contacts(0);
            data[10] = sval_mt_pad_contacts(1);
            break;
        default:
            memset(data + 1, 0, length - 1);
            data[1] = 1;
            break;
    }
}

// ---------------------------------------------------------------- output (firmware only)

#ifndef SVAL_MT_HOST_TEST
static int16_t wheel_h, wheel_v;
static uint8_t wheel_mods, wheel_state; // 0 idle, 1 queued for the pointing report, 2 sent

// Wheel keycodes are special-cased: a mousekey wheel tick is 1/120 of a notch
// with hi-res scroll, so put one full detent into the pointing report and hold
// the keycode's modifiers around it. Everything else is a keycode tap.
void tg_platform_emit(uint16_t kc) {
    uint8_t basic = kc & 0xFF;
    if (kc <= QK_MODS_MAX && basic >= QK_MOUSE_WHEEL_UP && basic <= QK_MOUSE_WHEEL_RIGHT) {
        uint8_t m5 = QK_MODS_GET_MODS(kc);
        uint8_t m8 = (m5 & 0x10) ? (uint8_t)((m5 & 0x0F) << 4) : m5;
        if (m8 & ~wheel_mods) {
            add_weak_mods(m8);
            send_keyboard_report();
        }
        wheel_mods |= m8;
#    if defined(POINTING_DEVICE_ENABLE) && defined(POINTING_DEVICE_HIRES_SCROLL_ENABLE)
        int16_t detent = (int16_t)pointing_device_get_hires_scroll_resolution();
#    else
        int16_t detent = 1;
#    endif
        if (basic == QK_MOUSE_WHEEL_UP) wheel_v += detent;
        else if (basic == QK_MOUSE_WHEEL_DOWN) wheel_v -= detent;
        else if (basic == QK_MOUSE_WHEEL_LEFT) wheel_h -= detent;
        else wheel_h += detent;
        wheel_state = 1;
    } else if (kc <= QK_MODS_MAX) {
        tap_code16(kc);
    } else {
        sval_keycode_tap(kc); // macros, layer keys and the rest, through process_record
    }
}

void tg_wheel_take(int16_t *h, int16_t *v) {
    if (wheel_state != 1) return;
    *h += wheel_h; *v += wheel_v;
    wheel_h = wheel_v = 0;
    wheel_state = 2;
}

// After the pointing report with the detent went out, let go of the modifiers.
void tg_task(void) {
    if (wheel_state != 2) return;
    if (wheel_mods) {
        del_weak_mods(wheel_mods);
        send_keyboard_report();
    }
    wheel_mods = wheel_state = 0;
}
#else
// Host tests capture outputs in tg_platform_emit; there is no pointing report.
void tg_wheel_take(int16_t *h, int16_t *v) { (void)h; (void)v; }
void tg_task(void) {}
#endif
