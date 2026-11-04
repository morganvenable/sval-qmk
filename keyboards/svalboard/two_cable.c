// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Two halves on separate USB cables, bridged by a host relay (prototype).
//
// Each half is its own USB keyboard. A host app (the relay) makes one half
// the lead and the other the follower:
//   follower: stops typing, and pushes its debounced key rows and pointer
//             motion to the relay over raw HID whenever they change;
//   lead:     takes those rows as the other half's rows, exactly where the
//             split link would put them, and runs the keymap for both halves.
// So layers, modifiers, combos, tap-hold and the pointing modes behave as
// they do over the split cable; only the transport is different.
//
// Both roles are leases held by the relay's packets. When the relay goes
// quiet, the lead releases the other half's keys and the follower types on
// its own again, as any half alone on USB does.
//
// Wire: [0xDD][client:4][0xFB][op] ... (64-byte reports, docs/two-cable.md).
#include "quantum.h"
#include "host.h"
#include "host_driver.h"
#include "client_wrapper.h"
#include "usb_device_state.h"
#include "two_cable.h"
#include "svalboard.h" // sval_other_half_connected
#ifdef SVAL_EITHER_HAND
#    include "side.h"
#endif
#include <string.h>

#define ROWS_PER_HAND (MATRIX_ROWS / 2)
_Static_assert(sizeof(matrix_row_t) == 1, "rows travel as one byte each");

#define FOLLOW_LEASE_MS 600
#define LEAD_LEASE_MS 500
#define CAPTURE_LEASE_MS 1000
#define HEARTBEAT_MS 100

enum { OP_FOLLOW = 0x01, OP_LEAD = 0x02, OP_RELEASE = 0x03, OP_STATUS = 0x04, OP_INJECT = 0x05, OP_CAPTURE = 0x06, OP_REBOOT = 0x07, OP_PEER = 0x08 };
enum { PUSH_ROWS = 0x90, PUSH_APPLIED = 0x92, PUSH_REPORT = 0x93 };
enum { ROLE_NONE, ROLE_LEAD, ROLE_FOLLOWER };
// Reply status: 0 OK, 1 not a follower (INJECT), 2 unknown op (and the bench
// ops in a build without SVAL_TWO_CABLE_BENCH), 3 the split cable works, so
// FOLLOW and LEAD are refused, 4 INJECT on a lead that is not capturing,
// 5 PEER on a half that is not leading.
enum { STATUS_OK, STATUS_NOT_FOLLOWER, STATUS_UNKNOWN_OP, STATUS_CABLE, STATUS_NOT_CAPTURING, STATUS_NOT_LEAD };

// INJECT, CAPTURE and REBOOT are for hardware tests: CAPTURE hands every
// report the lead makes to a raw HID client, and INJECT presses keys.
#ifdef SVAL_TWO_CABLE_BENCH
#    define BENCH_OPS true
#else
#    define BENCH_OPS false
#endif

extern matrix_row_t matrix[MATRIX_ROWS]; // quantum/matrix_common.c, debounced

static uint8_t  role;
static uint32_t owner;    // client ID of the relay
static uint32_t last_pkt; // its last packet for this role

// Relay down: this half led, and the relay went quiet without a RELEASE. The
// light waits RELAY_LIGHT_AFTER_MS, so a relay that restarts at once never
// blips, then repeats a double blip every RELAY_LIGHT_EVERY_MS.
#define RELAY_LIGHT_AFTER_MS 3000
#define RELAY_LIGHT_EVERY_MS 3000
#define RELAY_LIGHT_ON_MS 450 // the blip itself: update_led.c, UPDATE_LED_RELAY
static bool     relay_lapsed;
static uint32_t relay_lapsed_at;

// lead
static uint8_t  applied_seq;
static bool     applied_pending;
static int32_t  in_dx, in_dy, in_h, in_v;
static uint8_t  in_buttons;
static uint32_t lead_packets;
static uint8_t  lead_inject[ROWS_PER_HAND]; // bench: the lead's own keys, pressed by the relay
static bool     reboot_pending;
static uint32_t reboot_at;
// The follower's own identity blocks, from its Sval 0x2C and 0x38 replies
// (op PEER): where the split link's presence reply would put them.
static sval_pointer_identity_t peer_pointer;
static sval_board_identity_t   peer_board;

// follower
static uint8_t  sent_rows[ROWS_PER_HAND];
static uint8_t  sent_buttons;
static int32_t  acc_dx, acc_dy, acc_h, acc_v;
static uint8_t  acc_buttons;
static uint32_t last_push_ms, last_motion_push_us;
static uint8_t  push_seq;
static uint8_t  inject_rows[ROWS_PER_HAND];
static int16_t  inject_dx, inject_dy;

// capture (lead, bench only): reports go to the relay instead of the computer
static bool           capture_on;
static uint32_t       capture_owner, capture_seen;
static host_driver_t  capture_driver;
static host_driver_t *capture_saved;
#define CAPTURE_DRAIN_MS 1000
static bool     capture_draining;
static uint32_t capture_drain_at;

static inline uint8_t this_hand(void) {
    return is_keyboard_left() ? 0 : ROWS_PER_HAND;
}
static inline uint8_t that_hand(void) {
    return is_keyboard_left() ? ROWS_PER_HAND : 0;
}
static inline uint32_t now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}
static void put32(uint8_t *p, uint32_t v) {
    for (uint8_t i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}
static void put16(uint8_t *p, int16_t v) {
    p[0] = (uint16_t)v & 0xFF;
    p[1] = (uint16_t)v >> 8;
}
static int16_t get16s(const uint8_t *p) {
    return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint8_t my_side(void) {
#ifdef SVAL_EITHER_HAND
    return sval_side();
#else
    return is_keyboard_left() ? 0 : 1;
#endif
}

static void header(uint8_t *b, uint32_t client, uint8_t op) {
    memset(b, 0, RAW_EPSIZE);
    b[0] = WRAPPER_PREFIX;
    put32(&b[1], client);
    b[5] = SVAL_TWO_CABLE_PROTO;
    b[6] = op;
}

bool sval_two_cable_lead_live(void) {
    return role == ROLE_LEAD;
}
bool sval_two_cable_follower_live(void) {
    return role == ROLE_FOLLOWER;
}

#ifndef SVAL_KEYTEST
// A follower's own keys never reach its keymap; they go to the lead.
bool should_process_keypress(void) {
    return is_keyboard_master() && role != ROLE_FOLLOWER;
}
#endif

// ---- lead -----------------------------------------------------------------------

static void lead_clear_rows(void) {
    for (uint8_t r = 0; r < ROWS_PER_HAND; r++)
        matrix[that_hand() + r] = 0;
    in_dx = in_dy = in_h = in_v = 0;
    in_buttons                  = 0;
}

static void lead_drop_keys(void) {
    lead_clear_rows(); // the next matrix_task() releases every key the relay held
    for (uint8_t r = 0; r < ROWS_PER_HAND; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            if (lead_inject[r] & (1u << c)) action_exec(MAKE_KEYEVENT(this_hand() + r, c, false));
        }
        lead_inject[r] = 0;
    }
}

// The split link is up: the cable carries the other half, so the relay steps
// aside. Both roles refuse, and a live role ends. The presence ping sets the
// other half's side then, so ending the lead leaves it alone.
static bool cable_up(void) {
    return is_keyboard_master() && sval_other_half_connected();
}

// The other half is dropped (its side unknown) only after a matrix_task() has
// released the relayed keys: while it is gone, side.c throws away events from
// its rows, releases included, which would leave a held layer or tap-hold on.
// 1: set by lead_end(); the first task pass after that counts it on, and the
// next one (a whole keyboard_task() later) drops the side.
static uint8_t side_drop;

static void lead_end(void) {
    lead_drop_keys();
    memset(&peer_pointer, 0, sizeof(peer_pointer));
    memset(&peer_board, 0, sizeof(peer_board));
    if (!cable_up()) side_drop = 1;
    role = ROLE_NONE;
}

static void side_drop_task(void) {
    if (!side_drop || ++side_drop < 3) return;
    side_drop = 0;
#ifdef SVAL_EITHER_HAND
    if (role != ROLE_LEAD && !cable_up()) {
        for (uint8_t i = 0; i < 3; i++)
            sval_side_other_set(false, false, SVAL_SIDE_UNKNOWN);
    }
#endif
}

static void lead_state(const uint8_t *d) {
    // d[7] seq, d[8] follower side, d[9..13] rows, d[14..17] dx dy, d[18] h, d[19] v, d[20] buttons,
    // d[21] flags (bit 0: reply, bit 1: the follower's rows hold injected keys)
    bool changed  = false;
    bool injected = d[21] & 2;
    side_drop     = 0;
    for (uint8_t r = 0; r < ROWS_PER_HAND; r++) {
        // A draining capture keeps them up, and injected keys act only into a capture.
        matrix_row_t row = capture_draining || (injected && !capture_on) ? 0 : d[9 + r];
        if (matrix[that_hand() + r] != row) {
            matrix[that_hand() + r] = row;
            changed                 = true;
        }
    }
    in_dx += get16s(&d[14]);
    in_dy += get16s(&d[16]);
    in_h += (int8_t)d[18];
    in_v += (int8_t)d[19];
    in_buttons = d[20];
    if (changed) {
        applied_seq     = d[7];
        applied_pending = true;
    }
    lead_packets++;
#ifdef SVAL_EITHER_HAND
    uint8_t s = d[8];
    sval_side_other_set(true, s <= 1, s <= 1 ? s : SVAL_SIDE_UNKNOWN);
#endif
}

// ---- follower ---------------------------------------------------------------------

static void follower_push(bool force) {
    uint8_t rows[ROWS_PER_HAND];
    bool    changed = false;
    for (uint8_t r = 0; r < ROWS_PER_HAND; r++) {
        rows[r] = matrix_get_row(this_hand() + r) | inject_rows[r];
        if (rows[r] != sent_rows[r]) changed = true;
    }
    if (acc_buttons != sent_buttons) changed = true;
    bool motion = acc_dx || acc_dy || acc_h || acc_v;
    // Motion at most once per millisecond; keys at once.
    if (motion && !changed && (uint32_t)(now_us() - last_motion_push_us) < 1000) motion = false;
    if (!changed && !motion && !(force || timer_elapsed32(last_push_ms) >= HEARTBEAT_MS)) return;

    uint8_t b[RAW_EPSIZE];
    header(b, owner, PUSH_ROWS);
    b[7] = ++push_seq;
    b[8] = my_side();
    memcpy(&b[9], rows, ROWS_PER_HAND);
    int32_t dx = acc_dx < -32768 ? -32768 : acc_dx > 32767 ? 32767 : acc_dx;
    int32_t dy = acc_dy < -32768 ? -32768 : acc_dy > 32767 ? 32767 : acc_dy;
    int32_t h  = acc_h < -127 ? -127 : acc_h > 127 ? 127 : acc_h;
    int32_t v  = acc_v < -127 ? -127 : acc_v > 127 ? 127 : acc_v;
    put16(&b[14], dx);
    put16(&b[16], dy);
    b[18] = (uint8_t)(int8_t)h;
    b[19] = (uint8_t)(int8_t)v;
    b[20] = acc_buttons;
    put32(&b[21], now_us());
    b[25] = (inject_rows[0] | inject_rows[1] | inject_rows[2] | inject_rows[3] | inject_rows[4]) ? 1 : 0;
    acc_dx -= dx;
    acc_dy -= dy;
    acc_h -= h;
    acc_v -= v;
    memcpy(sent_rows, rows, ROWS_PER_HAND);
    sent_buttons = acc_buttons;
    last_push_ms = timer_read32();
    if (motion) last_motion_push_us = now_us();
    host_raw_hid_send(b, RAW_EPSIZE);
}

static void follower_begin(void) {
    clear_keyboard(); // nothing this half typed stays held on the computer
    layer_clear();
    memset(sent_rows, 0xFF, sizeof(sent_rows)); // the first push always goes
    acc_dx = acc_dy = acc_h = acc_v = 0;
    acc_buttons = sent_buttons = 0;
    memset(inject_rows, 0, sizeof(inject_rows));
    inject_dx = inject_dy = 0;
    role                  = ROLE_FOLLOWER;
}

static void follower_end(void) {
    memset(inject_rows, 0, sizeof(inject_rows));
    role = ROLE_NONE; // types on its own again from the next key change
}

static void role_end(void) {
    if (role == ROLE_LEAD) lead_end();
    if (role == ROLE_FOLLOWER) follower_end();
}

// ---- capture (bench) ---------------------------------------------------------------

static void capture_push(uint8_t kind, const uint8_t *bytes, uint8_t n) {
    uint8_t b[RAW_EPSIZE];
    header(b, capture_owner, PUSH_REPORT);
    b[7] = kind;
    put32(&b[8], now_us());
    if (n > RAW_EPSIZE - 13) n = RAW_EPSIZE - 13;
    b[12] = n;
    memcpy(&b[13], bytes, n);
    host_raw_hid_send(b, RAW_EPSIZE);
}
static void cap_keyboard(report_keyboard_t *r) {
    uint8_t x[1 + KEYBOARD_REPORT_KEYS];
    x[0] = r->mods;
    memcpy(&x[1], r->keys, KEYBOARD_REPORT_KEYS);
    capture_push(1, x, sizeof(x));
}
static void cap_nkro(report_nkro_t *r) {
    uint8_t x[1 + NKRO_REPORT_BITS];
    x[0] = r->mods;
    memcpy(&x[1], r->bits, NKRO_REPORT_BITS);
    capture_push(2, x, sizeof(x));
}
static void cap_mouse(report_mouse_t *r) {
    uint8_t x[9] = {r->buttons};
    put16(&x[1], r->x);
    put16(&x[3], r->y);
    put16(&x[5], r->h);
    put16(&x[7], r->v);
    capture_push(3, x, sizeof(x));
}
static void cap_extra(report_extra_t *r) {
    uint8_t x[3] = {r->report_id, r->usage & 0xFF, r->usage >> 8};
    capture_push(4, x, sizeof(x));
}

static void capture_begin(uint32_t client) {
    capture_owner = client;
    capture_seen  = timer_read32();
    if (capture_on) {
        capture_draining = false;
        return;
    }
    clear_keyboard();
    capture_saved                = host_get_driver();
    capture_driver               = *capture_saved; // LEDs and raw HID stay connected
    capture_driver.send_keyboard = cap_keyboard;
    capture_driver.send_nkro     = cap_nkro;
    capture_driver.send_mouse    = cap_mouse;
    capture_driver.send_extra    = cap_extra;
    host_set_driver(&capture_driver);
    capture_on = true;
}

// Ending a capture first lets go of every relayed and injected key and keeps
// capturing for a while, so a tap a release resolves (tap-hold, tap dance)
// lands in the capture too and never on the computer.
static void capture_end(void) {
    if (!capture_on || capture_draining) return;
    if (role == ROLE_LEAD) lead_drop_keys();
    capture_draining = true;
    capture_drain_at = timer_read32();
}

static void capture_drain_task(void) {
    if (!capture_draining || timer_elapsed32(capture_drain_at) < CAPTURE_DRAIN_MS) return;
    clear_keyboard(); // into the capture: the computer saw none of it
    host_set_driver(capture_saved);
    capture_on       = false;
    capture_draining = false;
}

// ---- commands and task ------------------------------------------------------------

bool sval_two_cable_command(uint8_t *data, uint8_t length) {
    if (length < 32) return false;
    uint32_t client = data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
    uint8_t  op     = data[6];
    uint8_t  status = STATUS_OK;
    if ((op == OP_FOLLOW || op == OP_LEAD) && cable_up()) {
        // The split cable carries the other half: no relay needed. Refused
        // with a reply (even for LEAD), so the relay can say so.
        role_end();
        status = STATUS_CABLE;
    } else if (!BENCH_OPS && (op == OP_INJECT || op == OP_CAPTURE || op == OP_REBOOT)) {
        status = STATUS_UNKNOWN_OP; // bench builds only: no other app may type, read keys or reboot
    } else if (op == OP_INJECT && role == ROLE_LEAD && !capture_on) {
        status = STATUS_NOT_CAPTURING; // the lead's own injected keys act only into a capture
    } else
        switch (op) {
            case OP_FOLLOW:
                if (role == ROLE_LEAD) lead_end();
                if (role != ROLE_FOLLOWER) follower_begin();
                owner    = client;
                last_pkt = timer_read32();
                follower_push(true);
                break;
            case OP_LEAD:
                if (role == ROLE_FOLLOWER) follower_end();
                role     = ROLE_LEAD;
                owner    = client;
                last_pkt = timer_read32();
                lead_state(data);
                if (!(data[21] & 1)) return false; // no reply unless asked
                break;
            case OP_RELEASE:
                role_end();
                capture_end();
                break;
            case OP_STATUS:
                break;
            case OP_PEER:
                // d[7] blocks held (bit 0 pointer, bit 1 board), d[8..12] the
                // follower's 0x2C block for itself, d[13..17] its 0x38 block.
                if (role != ROLE_LEAD) {
                    status = STATUS_NOT_LEAD;
                    break;
                }
                memset(&peer_pointer, 0, sizeof(peer_pointer));
                memset(&peer_board, 0, sizeof(peer_board));
                if (data[7] & 1) memcpy(&peer_pointer, &data[8], sizeof(peer_pointer));
                if (data[7] & 2) memcpy(&peer_board, &data[8 + sizeof(peer_pointer)], sizeof(peer_board));
                break;
            case OP_INJECT:
                if (role == ROLE_LEAD) {
                    // Bench only: press or release the lead's own keys, as keytest does.
                    for (uint8_t r = 0; r < ROWS_PER_HAND; r++) {
                        uint8_t diff = lead_inject[r] ^ data[7 + r];
                        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                            if (diff & (1u << c)) action_exec(MAKE_KEYEVENT(this_hand() + r, c, (data[7 + r] >> c) & 1));
                        }
                        lead_inject[r] = data[7 + r];
                    }
                    break;
                }
                if (role != ROLE_FOLLOWER) {
                    status = STATUS_NOT_FOLLOWER;
                    break;
                }
                memcpy(inject_rows, &data[7], ROWS_PER_HAND);
                inject_dx = get16s(&data[12]);
                inject_dy = get16s(&data[14]);
                acc_dx += inject_dx;
                acc_dy += inject_dy;
                follower_push(true);
                break;
            case OP_REBOOT: // bench: drop off USB and come back, as a pulled cable does to the relay
                reboot_pending = true;
                reboot_at      = timer_read32() + 100; // reply first
                break;
            case OP_CAPTURE:
                if (data[7])
                    capture_begin(client);
                else
                    capture_end();
                break;
            default:
                status = STATUS_UNKNOWN_OP;
                break;
        }
    // Any role a relay asks for, or a RELEASE, means a relay is there again.
    if (op == OP_FOLLOW || op == OP_LEAD || op == OP_RELEASE) relay_lapsed = false;
    uint8_t seq = data[7];
    memset(&data[7], 0, length - 7);
    data[6]  = 0x80 | op;
    data[7]  = seq;
    data[8]  = status;
    data[9]  = role;
    data[10] = my_side();
    data[11] = capture_on;
    put32(&data[12], lead_packets);
    put32(&data[16], now_us());
    return true;
}

void sval_two_cable_task(void) {
    if (reboot_pending && timer_expired32(timer_read32(), reboot_at)) soft_reset_keyboard();
    if (capture_on && timer_elapsed32(capture_seen) > CAPTURE_LEASE_MS) capture_end();
    capture_drain_task();
    side_drop_task();
    if (role != ROLE_NONE && cable_up()) {
        role_end(); // the cable came back: it carries the other half again
        return;
    }
    if (role == ROLE_LEAD) {
        if (timer_elapsed32(last_pkt) > LEAD_LEASE_MS) {
            lead_end();
            relay_lapsed    = true; // after lead_end(): ending a role on request clears it
            relay_lapsed_at = timer_read32();
            return;
        }
        if (applied_pending) {
            // matrix_task() has run since the rows arrived: report what the keymap made of them.
            applied_pending = false;
            uint8_t b[RAW_EPSIZE];
            header(b, owner, PUSH_APPLIED);
            b[7] = applied_seq;
            put32(&b[8], layer_state);
            put32(&b[12], default_layer_state);
            b[16] = get_mods();
            b[17] = get_weak_mods();
            b[18] = get_oneshot_mods();
            put32(&b[19], now_us());
            host_raw_hid_send(b, RAW_EPSIZE);
        }
    } else if (role == ROLE_FOLLOWER) {
        if (timer_elapsed32(last_pkt) > FOLLOW_LEASE_MS) {
            follower_end();
            return;
        }
        follower_push(false);
    }
}

bool sval_two_cable_relay_light(void) {
    if (!relay_lapsed) return false;
    if (cable_up()) { // the split cable carries the other half: nothing is missing
        relay_lapsed = false;
        return false;
    }
    if (usb_device_state_get_configure_state() != USB_DEVICE_STATE_CONFIGURED) return false; // asleep or unplugged
    uint32_t t = timer_elapsed32(relay_lapsed_at);
    return t >= RELAY_LIGHT_AFTER_MS && (t - RELAY_LIGHT_AFTER_MS) % RELAY_LIGHT_EVERY_MS < RELAY_LIGHT_ON_MS;
}

bool sval_two_cable_peer_pointer(sval_pointer_identity_t *id) {
    if (role != ROLE_LEAD || !(peer_pointer.flags & SVAL_POINTER_REPORTED)) return false;
    *id = peer_pointer;
    return true;
}

bool sval_two_cable_peer_board(sval_board_identity_t *id) {
    if (role != ROLE_LEAD || !(peer_board.flags & SVAL_BOARD_REPORTED)) return false;
    *id = peer_board;
    return true;
}

void sval_two_cable_pointing(report_mouse_t *left, report_mouse_t *right) {
    report_mouse_t *mine  = is_keyboard_left() ? left : right;
    report_mouse_t *other = is_keyboard_left() ? right : left;
    if (role == ROLE_FOLLOWER) {
        acc_dx += mine->x;
        acc_dy += mine->y;
        acc_h += mine->h;
        acc_v += mine->v;
        acc_buttons = mine->buttons;
        *left = *right = (report_mouse_t){0};
    } else if (role == ROLE_LEAD) {
        int32_t lim    = MOUSE_REPORT_XY_MAX;
        int32_t dx     = in_dx < -lim ? -lim : in_dx > lim ? lim : in_dx;
        int32_t dy     = in_dy < -lim ? -lim : in_dy > lim ? lim : in_dy;
        int32_t h      = in_h < -127 ? -127 : in_h > 127 ? 127 : in_h;
        int32_t v      = in_v < -127 ? -127 : in_v > 127 ? 127 : in_v;
        other->x       = dx;
        other->y       = dy;
        other->h       = h;
        other->v       = v;
        other->buttons = in_buttons;
        in_dx -= dx;
        in_dy -= dy;
        in_h -= h;
        in_v -= v;
    }
}
