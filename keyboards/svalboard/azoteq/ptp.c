// SPDX-License-Identifier: GPL-2.0-or-later
// Native Precision Touchpad (experiment/native-ptp).
//
// The USB half merges both IQS5xx pads into one surface and reports it on its
// own HID interface (Digitizers 0x0D, Touch Pad 0x05). Windows or Linux then
// own gesture recognition. This file owns only what is fed in: geometry,
// contact IDs, arming, the 5-contact limit, lift, palm, faults and pacing.
//
// Threads: the feature-report setters run in the USB ISR and only store
// values (f_*). Everything else runs in the main loop.
#ifdef SVAL_PTP_HOST_TEST
#    include "ptp_test_platform.h"
#else
#    include <ch.h>
#    include <hal.h>
#    include <string.h>
#    include "usb_main.h"
#    include "usb_driver.h"
#    include "usb_endpoints.h"
#endif
#include "ptp.h"
#include "ptp_descriptor.h"

#ifndef SVAL_PTP_HOST_TEST
#    if CH_CFG_ST_FREQUENCY != 1000000
#        error "ptp.c counts scan time on a 1 MHz system timer"
#    endif
extern usb_endpoint_in_t usb_endpoints_in[USB_ENDPOINT_IN_COUNT];

static uint32_t ptp_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

// Post one whole report or nothing; never wait (see multitouch.c try_send).
static bool ptp_try_send(const uint8_t *report) {
    usb_endpoint_in_t      *endpoint = &usb_endpoints_in[USB_ENDPOINT_IN_PTP];
    output_buffers_queue_t *q        = &endpoint->obqueue;
    bool                    sent     = false;
    osalSysLock();
    if (usbGetDriverStateI(endpoint->config.usbp) == USB_ACTIVE && !q->suspended && q->ptr == NULL && !obqIsFullI(q) && obqGetEmptyBufferTimeoutS(q, TIME_IMMEDIATE) == MSG_OK) {
        memcpy(q->ptr, report, SVAL_PTP_INPUT_SIZE);
        obqPostFullBufferS(q, SVAL_PTP_INPUT_SIZE);
        sent = true;
    }
    osalSysUnlock();
    return sent;
}
#    define PTP_LOCK() osalSysLock()
#    define PTP_UNLOCK() osalSysUnlock()
#endif

// Microsoft's published default certification blob for Precision Touchpad
// development (Windows Precision Touchpad sample descriptor, feature report
// 0xC5). Not a Svalboard certificate; it only satisfies the inbox driver.
static const uint8_t cert[SVAL_PTP_CERT_SIZE] = {
    SVAL_PTP_REPORT_ID_CERT,
    0xFC, 0x28, 0xFE, 0x84, 0x40, 0xCB, 0x9A, 0x87, 0x0D, 0xBE, 0x57, 0x3C, 0xB6, 0x70, 0x09, 0x88,
    0x07, 0x97, 0x2D, 0x2B, 0xE3, 0x38, 0x34, 0xB6, 0x6C, 0xED, 0xB0, 0xF7, 0xE5, 0x9C, 0xF6, 0xC2,
    0x2E, 0x84, 0x1B, 0xE8, 0xB4, 0x51, 0x78, 0x43, 0x1F, 0x28, 0x4B, 0x7C, 0x2D, 0x53, 0xAF, 0xFC,
    0x47, 0x70, 0x1B, 0x59, 0x6F, 0x74, 0x43, 0xC4, 0xF3, 0x47, 0x18, 0x53, 0x1A, 0xA2, 0xA1, 0x71,
    0xC7, 0x95, 0x0E, 0x31, 0x55, 0x21, 0xD3, 0xB5, 0x1E, 0xE9, 0x0C, 0xBA, 0xEC, 0xB8, 0x89, 0x19,
    0x3E, 0xB3, 0xAF, 0x75, 0x81, 0x9D, 0x53, 0xB9, 0x41, 0x57, 0xF4, 0x6D, 0x39, 0x25, 0x29, 0x7C,
    0x87, 0xD9, 0xB4, 0x98, 0x45, 0x7D, 0xA7, 0x26, 0x9C, 0x65, 0x3B, 0x85, 0x68, 0x89, 0xD7, 0x3B,
    0xBD, 0xFF, 0x14, 0x67, 0xF2, 0x2B, 0xF0, 0x2A, 0x41, 0x54, 0xF0, 0xFD, 0x2C, 0x66, 0x7C, 0xF8,
    0xC0, 0x8F, 0x33, 0x13, 0x03, 0xF1, 0xD3, 0xC1, 0x0B, 0x89, 0xD9, 0x1B, 0x62, 0xCD, 0x51, 0xB7,
    0x80, 0xB8, 0xAF, 0x3A, 0x10, 0xC1, 0x8A, 0x5B, 0xE8, 0x8A, 0x56, 0xF0, 0x8C, 0xAA, 0xFA, 0x35,
    0xE9, 0x42, 0xC4, 0xD8, 0x55, 0xC3, 0x38, 0xCC, 0x2B, 0x53, 0x5C, 0x69, 0x52, 0xD5, 0xC8, 0x73,
    0x02, 0x38, 0x7C, 0x73, 0xB6, 0x41, 0xE7, 0xFF, 0x05, 0xD8, 0x2B, 0x79, 0x9A, 0xE2, 0x34, 0x60,
    0x8F, 0xA3, 0x32, 0x1F, 0x09, 0x78, 0x62, 0xBC, 0x80, 0xE3, 0x0F, 0xBD, 0x65, 0x20, 0x08, 0x13,
    0xC1, 0xE2, 0xEE, 0x53, 0x2D, 0x86, 0x7E, 0xA7, 0x5A, 0xC5, 0xD3, 0x7D, 0x98, 0xBE, 0x31, 0x48,
    0x1F, 0xFB, 0xDA, 0xAF, 0xA2, 0xA8, 0x6A, 0x89, 0xD6, 0xBF, 0xF2, 0xD3, 0x32, 0x2A, 0x9A, 0xE4,
    0xCF, 0x17, 0xB7, 0xB8, 0xF4, 0xE1, 0x33, 0x08, 0x24, 0x8B, 0xC4, 0x43, 0xA5, 0xE5, 0x24, 0xC2,
};

// ---------------------------------------------------------------- state

// Written by the USB ISR (feature reports, reset); read in the main loop.
static volatile uint8_t f_mode, f_latency, f_switches = 3;
static volatile bool    f_reset;

enum { C_FREE, C_CANDIDATE, C_ACTIVE, C_IGNORED };
typedef struct {
    uint8_t  state, pad, slot, life, cid;
    bool     conf;      // Confidence; once cleared it stays cleared
    bool     sent_down; // reported with Tip = 1 at least once
    bool     lift;      // gone: one Tip = 0 frame (after one Tip = 1 if never sent), then free
    bool     palm;      // chip palm flag: one Confidence = 0 frame, then lift
    uint16_t x, y;
    uint32_t order;
} contact_t;
#define POOL 16
static contact_t pool[POOL];
static uint32_t  arrival;

typedef struct {
    bool     known, up, armed, has_epoch;
    uint32_t epoch;
} pad_state_t;
static pad_state_t pads[2];

static bool     native, mode3, output, hold, release_pending, dirty, ever_sent, scan_started;
static uint32_t release_at, last_sent, scan_clock, scan_rem, frames_sent, send_busy;
static uint16_t cid_busy, scan;
static uint8_t  cid_next, key_buttons, sent_buttons, seen_switches = 3;
#if SVAL_PTP_KEY_BUTTONS
static uint8_t key_taken; // buttons whose press went to the touchpad
#endif
static uint16_t motion[2];
static uint8_t  last_report[SVAL_PTP_INPUT_SIZE] = {SVAL_PTP_REPORT_ID_INPUT};

static uint16_t ptp_get16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t ptp_get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void ptp_put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
static void ptp_put32(uint8_t *p, uint32_t v) {
    ptp_put16(p, v);
    ptp_put16(p + 2, v >> 16);
}

// --------------------------------------------------------------- contacts

static void free_contact(contact_t *c) {
    if (c->state == C_ACTIVE) cid_busy &= ~(1u << c->cid);
    memset(c, 0, sizeof(*c));
}

static void clear_all(void) {
    for (uint8_t i = 0; i < POOL; i++) free_contact(&pool[i]);
    pads[0].armed = pads[1].armed = false;
    dirty = false;
}

static contact_t *alloc_contact(void) {
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_FREE) return &pool[i];
    return NULL;
}

static uint8_t take_cid(void) {
    for (uint8_t k = 0; k < 16; k++) {
        uint8_t id = (cid_next + k) & 15;
        if (!(cid_busy & (1u << id))) {
            cid_busy |= 1u << id;
            cid_next = (id + 1) & 15; // round robin: a freed ID is not reused at once
            return id;
        }
    }
    return 0; // unreachable: at most 5 contacts hold an ID
}

static void end_contact(contact_t *c) {
    if (c->state == C_ACTIVE || c->state == C_CANDIDATE) {
        c->lift = true;
        dirty   = true;
    } else {
        free_contact(c);
    }
}

// Reset, recalibration, overflow or a stale remote: lift only that pad.
static void lift_pad(uint8_t pad) {
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad) continue;
        if (c->state == C_ACTIVE && c->sent_down) {
            c->lift = true;
            c->palm = false;
            dirty   = true;
        } else {
            free_contact(c);
        }
    }
    pads[pad].armed = false;
}

// The chip's palm or too-many flag. Its frame carries no positions, so report
// the reported contacts once with Confidence = 0, then lift them.
static void palm_pad(uint8_t pad) {
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad) continue;
        if (c->state == C_ACTIVE && c->sent_down) {
            c->conf = false;
            if (!c->lift) c->palm = true;
            dirty = true;
        } else {
            free_contact(c);
        }
    }
    pads[pad].armed = false;
}

static void map_xy(uint8_t pad, const uint8_t *slot, uint16_t *x, uint16_t *y) {
    uint16_t rx = ptp_get16(slot + 1), ry = ptp_get16(slot + 3);
    if (rx > SVAL_PTP_PAD_X - 1) rx = SVAL_PTP_PAD_X - 1;
    if (ry > SVAL_PTP_PAD_Y - 1) ry = SVAL_PTP_PAD_Y - 1;
    if (pad ? SVAL_PTP_FLIP_R_X : SVAL_PTP_FLIP_L_X) rx = SVAL_PTP_PAD_X - 1 - rx;
    if (pad ? SVAL_PTP_FLIP_R_Y : SVAL_PTP_FLIP_L_Y) ry = SVAL_PTP_PAD_Y - 1 - ry;
    *x = rx + pad * SVAL_PTP_PAD_STRIDE;
    *y = ry;
}

static uint16_t distance(const contact_t *c, uint16_t x, uint16_t y) {
    uint16_t dx = c->x > x ? c->x - x : x - c->x;
    uint16_t dy = c->y > y ? c->y - y : y - c->y;
    return dx + dy;
}

static contact_t *find_contact(uint8_t pad, uint8_t slot, uint8_t life) {
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state != C_FREE && !c->lift && c->pad == pad && c->slot == slot && c->life == life) return c;
    }
    return NULL;
}

void sval_ptp_feed(const uint8_t *f) {
    if (f[0] != 1) return;
    uint8_t      pad = f[1] ? 1 : 0, flags = f[2] & 0x7F, mask = f[3] & 0x1F;
    uint32_t     epoch = ptp_get32(f + 12);
    pad_state_t *p     = &pads[pad];

    if (flags & 0x20) { // remote snapshot stale
        sval_ptp_pad_lost(pad);
        return;
    }
    if (flags & 0x08) { // chip reset or recalibration
        lift_pad(pad);
        p->known = true;
        p->up    = false;
        return;
    }
    if (flags & 0x03) { // palm or too many fingers
        palm_pad(pad);
        p->known = true;
        p->up    = false;
        return;
    }
    if (flags & 0x04) return; // invalid read: skip it and hold the last state

    // 0x10 (the stream restarted) and plain frames take the slot rules below.
    p->known       = true;
    p->up          = mask == 0;
    bool new_epoch = p->has_epoch && epoch != p->epoch;
    p->epoch       = epoch;
    p->has_epoch   = true;
    if (!output) return;
    if (!p->armed) {
        if (mask == 0) p->armed = true; // one all-up sample arms the pad
        return;
    }

    // Contacts that left their slot, or whose slot now holds another contact.
    // After a stream restart, lifetime IDs restart too: keep a contact whose
    // slot is still occupied close to where it was.
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_FREE || c->pad != pad || c->lift) continue;
        const uint8_t *s = f + 16 + 8 * c->slot;
        if (!(mask & (1u << c->slot))) {
            end_contact(c);
            continue;
        }
        if (new_epoch) { // lifetime IDs restarted: match by slot and position
            uint16_t x, y;
            map_xy(pad, s, &x, &y);
            if (c->state != C_IGNORED && distance(c, x, y) <= SVAL_PTP_REMAP_COUNTS)
                c->life = s[0];
            else
                end_contact(c);
        } else if (s[0] != c->life) {
            end_contact(c);
        }
    }

    for (uint8_t slot = 0; slot < 5; slot++) {
        if (!(mask & (1u << slot))) continue;
        const uint8_t *s = f + 16 + 8 * slot;
        uint16_t       x, y;
        map_xy(pad, s, &x, &y);
        bool       palm_area = SVAL_PTP_PALM_AREA && s[7] > SVAL_PTP_PALM_AREA;
        contact_t *c         = find_contact(pad, slot, s[0]);
        if (c) {
            if (c->state == C_IGNORED) continue;
            if (c->state == C_ACTIVE) {
                uint32_t m = (uint32_t)motion[pad] + distance(c, x, y);
                motion[pad] = m > 0xFFFF ? 0xFFFF : m;
            }
            c->x = x;
            c->y = y;
            if (palm_area) c->conf = false;
            dirty = true; // every sample with a finger down is a frame
            continue;
        }
        c = alloc_contact();
        if (!c) continue; // pool full: never reported
        c->state = C_CANDIDATE;
        c->pad   = pad;
        c->slot  = slot;
        c->life  = s[0];
        c->x     = x;
        c->y     = y;
        c->conf  = !palm_area;
        c->order = ++arrival;
        dirty    = true;
    }
}

void sval_ptp_pad_lost(uint8_t pad) {
    pad &= 1;
    lift_pad(pad);
    pads[pad].known     = false; // unknown counts as up for the mouse hold
    pads[pad].has_epoch = false;
}

// Admit waiting contacts in arrival order. A contact that finds 5 others
// already down is ignored for its whole lifetime (Microsoft's rule). One that
// only waits for a lifting contact's slot is admitted on a later frame.
static void admit(void) {
    uint8_t live = 0, slots = 0;
    for (uint8_t i = 0; i < POOL; i++) {
        if (pool[i].state != C_ACTIVE) continue;
        slots++;
        if (!pool[i].lift) live++;
    }
    for (;;) {
        contact_t *best = NULL;
        for (uint8_t i = 0; i < POOL; i++)
            if (pool[i].state == C_CANDIDATE && (!best || pool[i].order < best->order)) best = &pool[i];
        if (!best) break;
        if (live >= SVAL_PTP_MAX_CONTACTS) {
            if (best->lift)
                free_contact(best);
            else
                best->state = C_IGNORED;
            continue;
        }
        if (slots >= SVAL_PTP_MAX_CONTACTS) break;
        best->cid   = take_cid();
        best->state = C_ACTIVE;
        slots++;
        if (!best->lift) live++;
    }
}

static void build(uint8_t *r, bool release) {
    memset(r, 0, SVAL_PTP_INPUT_SIZE);
    r[0]      = SVAL_PTP_REPORT_ID_INPUT;
    uint8_t n = 0;
    for (uint8_t i = 0; i < POOL && n < SVAL_PTP_MAX_CONTACTS; i++) {
        const contact_t *c = &pool[i];
        if (c->state != C_ACTIVE) continue;
        if (release && !c->sent_down) continue; // never reported: nothing to release
        bool     tip = release ? false : c->palm ? true : !(c->lift && c->sent_down);
        uint8_t *o   = r + 1 + 6 * n++;
        o[0]         = (c->conf ? 1 : 0) | (tip ? 2 : 0);
        o[1]         = c->cid;
        ptp_put16(o + 2, c->x);
        ptp_put16(o + 4, c->y);
    }
    r[33] = n;
    r[34] = (!release && (f_switches & 2)) ? (key_buttons & 7) : 0;
}

static void commit(bool release) {
    dirty = false;
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state != C_ACTIVE) continue;
        if (release) {
            free_contact(c);
        } else if (c->palm) {
            c->palm      = false;
            c->lift      = true;
            c->sent_down = true;
            dirty        = true;
        } else if (c->lift && c->sent_down) {
            free_contact(c);
        } else {
            if (c->lift) dirty = true; // a tap shorter than a frame: Tip = 0 next
            c->sent_down = true;
        }
    }
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_CANDIDATE) dirty = true; // waiting for a slot
}

// Scan time: 100 us units on this half's 1 MHz clock, wrapping at 16 bits,
// strictly increasing from one sent frame to the next.
static uint16_t stamp(uint32_t now) {
    if (!scan_started) {
        scan_started = true;
        scan_clock   = now;
    }
    uint32_t d = now - scan_clock + scan_rem;
    scan_clock = now;
    scan += d / 100;
    scan_rem = d % 100;
    if (ever_sent && scan == ptp_get16(last_report + 31)) scan++;
    return scan;
}

static bool send_frame(uint32_t now, bool release) {
    uint8_t r[SVAL_PTP_INPUT_SIZE];
    if (!release) admit();
    build(r, release);
    ptp_put16(r + 31, stamp(now));
    if (!ptp_try_send(r)) {
        send_busy++;
        return false;
    }
    commit(release);
    last_sent = now;
    ever_sent = true;
    frames_sent++;
    sent_buttons = r[34];
    PTP_LOCK();
    memcpy(last_report, r, sizeof(r));
    PTP_UNLOCK();
    return true;
}

static void start_release(uint32_t now) {
    bool any = sent_buttons != 0;
    for (uint8_t i = 0; i < POOL; i++) {
        contact_t *c = &pool[i];
        if (c->state == C_ACTIVE && c->sent_down)
            any = true;
        else if (c->state != C_FREE)
            free_contact(c);
    }
    pads[0].armed = pads[1].armed = false;
    release_pending               = any;
    release_at                    = now;
    if (!any) clear_all();
}

static bool all_up(void) {
    for (uint8_t i = 0; i < 2; i++)
        if (pads[i].known && !pads[i].up) return false;
    return true;
}

static bool any_down(void) {
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_ACTIVE || pool[i].state == C_CANDIDATE) return true;
    return false;
}

void sval_ptp_task(void) {
    uint32_t now = ptp_now_us();
    if (f_reset) {
        // USB reset or SET_CONFIGURATION: the host starts over in mouse mode.
        PTP_LOCK();
        f_reset = false;
        PTP_UNLOCK();
        if (mode3) hold = true;
        clear_all();
        release_pending = false;
        output = mode3 = false;
        sent_buttons    = 0;
    }

    bool m3 = native && f_mode == 3;
    if (mode3 && !m3) hold = true; // the pad mouse resumes once all fingers are up
    mode3    = m3;
    bool out = m3 && (f_switches & 1);
    if (out != output) {
        if (out) {
            // Both pads arm again before PTP reports start. A release still
            // pending (output toggled off and on quickly) goes out first.
            if (release_pending)
                pads[0].armed = pads[1].armed = false;
            else
                clear_all();
        } else {
            start_release(now);
        }
        output = out;
    }
    if (hold && !mode3 && all_up()) hold = false;
    // A Button Switch change while reporting re-sends the buttons byte, so a
    // held button is released (switch off) or restored (switch on) at once.
    uint8_t switches = f_switches;
    if (output && ((switches ^ seen_switches) & 2) && (key_buttons || sent_buttons)) dirty = true;
    seen_switches = switches;

    bool paced = !ever_sent || now - last_sent >= SVAL_PTP_MIN_FRAME_US;
    if (release_pending) {
        if (now - release_at > 100000) { // the host stopped listening
            clear_all();
            release_pending = false;
        } else if (paced && send_frame(now, true)) {
            clear_all();
            release_pending = false;
        }
        return;
    }
    if (!output || !paced) return;
    bool keepalive = any_down() && now - last_sent >= SVAL_PTP_KEEPALIVE_MS * 1000u;
    if (dirty || keepalive) send_frame(now, false);
}

// ------------------------------------------------------------ coordination

void sval_ptp_set_native(bool n) {
    if (native && !n && mode3) hold = true;
    native = n;
}

bool sval_ptp_wants_frames(void) {
    return native || hold;
}

bool sval_ptp_active(void) {
    return native && f_mode == 3;
}

bool sval_ptp_suppress_mouse(void) {
    // mode3 covers the loop between the host leaving mode 3 and the task
    // setting hold, so the pad mouse never moves under a finger in between.
    return (native && (f_mode == 3 || mode3 || !SVAL_PTP_MOUSE_BEFORE_MODE3)) || hold;
}

uint16_t sval_ptp_take_motion(uint8_t pad) {
    uint16_t m        = motion[pad & 1];
    motion[pad & 1] = 0;
    return m;
}

bool sval_ptp_key_button(uint8_t button, bool pressed) {
#if SVAL_PTP_KEY_BUTTONS
    // Pad Type 2 (external buttons): Microsoft's PTP spec puts the primary
    // (left) external button on Button 2 and the secondary (right) on
    // Button 3; Button 1 is the clickpad's integrated button. Linux
    // hid-multitouch (WIN8_PTP_BUTTONS) maps them the same way. MS_BTN3
    // (middle) has no PTP usage, so it stays on the mouse report.
    if (button > 1) return false;
    uint8_t bit = 2u << button;
    if (pressed) {
        if (!sval_ptp_active()) return false;
        key_buttons |= bit;
        key_taken |= bit;
        dirty = true;
        return true;
    }
    if (!(key_taken & bit)) return false; // its press went to the mouse report
    key_taken &= ~bit;
    key_buttons &= ~bit;
    dirty = true;
    return true;
#else
    (void)button;
    (void)pressed;
    return false;
#endif
}

void sval_ptp_status(uint8_t *o) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < POOL; i++)
        if (pool[i].state == C_ACTIVE) n++;
    o[0] = native;
    o[1] = f_mode;
    o[2] = f_latency;
    o[3] = f_switches;
    o[4] = (output ? 1 : 0) | (hold ? 2 : 0) | (pads[0].armed ? 4 : 0) | (pads[1].armed ? 8 : 0) | (release_pending ? 16 : 0) | (sval_ptp_suppress_mouse() ? 32 : 0) | (mode3 ? 64 : 0);
    o[5] = n;
    ptp_put32(o + 6, frames_sent);
    ptp_put32(o + 10, send_busy);
    ptp_put16(o + 14, scan);
}

// --------------------------------------------------------- feature reports
// These run in the USB ISR: they only read or store single bytes.

uint16_t sval_ptp_get_report(uint8_t report_type, uint8_t report_id, const uint8_t **data) {
    static uint8_t reply[2];
    if (report_type == 1) {
        if (report_id != SVAL_PTP_REPORT_ID_INPUT) return 0;
        *data = last_report;
        return SVAL_PTP_INPUT_SIZE;
    }
    if (report_type != 3) return 0;
    reply[0] = report_id;
    switch (report_id) {
        case SVAL_PTP_REPORT_ID_CAPS:
            reply[1] = SVAL_PTP_MAX_CONTACTS | (2 << 4); // pad type 2: external buttons
            break;
        case SVAL_PTP_REPORT_ID_CERT:
            *data = cert;
            return sizeof(cert);
        case SVAL_PTP_REPORT_ID_LATENCY:
            reply[1] = f_latency;
            break;
        case SVAL_PTP_REPORT_ID_MODE:
            reply[1] = f_mode;
            break;
        case SVAL_PTP_REPORT_ID_SWITCHES:
            reply[1] = f_switches;
            break;
        default:
            return 0;
    }
    *data = reply;
    return 2;
}

bool sval_ptp_set_report_allowed(uint8_t report_type, uint8_t report_id, uint16_t length) {
    return report_type == 3 && length == 2 && report_id >= SVAL_PTP_REPORT_ID_LATENCY && report_id <= SVAL_PTP_REPORT_ID_SWITCHES;
}

bool sval_ptp_set_report(uint8_t report_type, uint8_t report_id, const uint8_t *data, uint16_t length) {
    if (!sval_ptp_set_report_allowed(report_type, report_id, length) || data[0] != report_id) return false;
    switch (report_id) {
        case SVAL_PTP_REPORT_ID_LATENCY:
            f_latency = data[1] & 1; // informational only
            break;
        case SVAL_PTP_REPORT_ID_MODE:
            f_mode = data[1]; // 3 touchpad, anything else mouse
            break;
        case SVAL_PTP_REPORT_ID_SWITCHES:
            f_switches = data[1] & 3;
            break;
    }
    return true;
}

void sval_ptp_usb_reset(void) {
    f_mode     = 0;
    f_latency  = 0;
    f_switches = 3;
    f_reset    = true;
}
