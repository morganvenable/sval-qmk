// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Optional on-device action-path instrumentation. No persistent test state.
#include "quantum.h"
#include "host.h"
#include "via.h"
#include "dynamic_keymap.h"
#include "keytest.h"
#include <string.h>

#define EVENT_CAPACITY 32
#define REPORT_CAPACITY 64
#define REPORT_BYTES 32
#define RESPONSE_BYTES 22 // Fits both bare VIA and its 6-byte client wrapper.
#define WATCHDOG_MS 30000

enum { INFO, BEGIN, ENQUEUE, RUN, READ, CLEAR, ABORT, REBOOT, STATE, SELECT_LAYER, ACK };
enum { OK, INVALID, INACTIVE, BUSY, FULL, MISSING, ABORTED };
enum { KEYBOARD = 1, NKRO, MOUSE, EXTRA, INPUT };
typedef struct { uint16_t delay; uint8_t row, col, pressed; } test_event_t;
typedef struct { uint32_t time; uint8_t kind, length, bytes[REPORT_BYTES]; } capture_t;
static test_event_t events[EVENT_CAPACITY];
static capture_t reports[REPORT_CAPACITY];
static matrix_row_t held[MATRIX_ROWS], projected[MATRIX_ROWS];
static uint8_t head, count;
static uint32_t first_report, next_report, lost_reports, deadline, last_command, reboot_at;
static bool active, running, aborted, reboot_pending, reboot_bootloader;
static host_driver_t capture_driver;

static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put32(uint8_t *p, uint32_t n) { for (uint8_t i = 0; i < 4; ++i) p[i] = n >> (8 * i); }

static void capture(uint8_t kind, const void *bytes, uint8_t length) {
    if (next_report - first_report == REPORT_CAPACITY) { ++first_report; ++lost_reports; }
    capture_t *r = &reports[next_report++ % REPORT_CAPACITY];
    r->time = timer_read32();
    r->kind = kind;
    r->length = length;
    memcpy(r->bytes, bytes, length);
}
static void capture_keyboard_report(report_keyboard_t *r) {
    uint8_t bytes[1 + KEYBOARD_REPORT_KEYS];
    bytes[0] = r->mods;
    memcpy(bytes + 1, r->keys, KEYBOARD_REPORT_KEYS);
    capture(KEYBOARD, bytes, sizeof(bytes));
}
static void capture_nkro_report(report_nkro_t *r) {
    uint8_t bytes[1 + NKRO_REPORT_BITS];
    bytes[0] = r->mods;
    memcpy(bytes + 1, r->bits, NKRO_REPORT_BITS);
    capture(NKRO, bytes, sizeof(bytes));
}
static void mouse_report(report_mouse_t *r) {
    uint8_t bytes[9] = {r->buttons};
    int16_t axes[] = {r->x, r->y, r->h, r->v};
    for (uint8_t i = 0; i < 4; ++i) { bytes[1 + 2 * i] = axes[i]; bytes[2 + 2 * i] = (uint16_t)axes[i] >> 8; }
    capture(MOUSE, bytes, sizeof(bytes));
}
static void extra_report(report_extra_t *r) {
    uint8_t bytes[] = {r->report_id, r->usage & 0xFF, r->usage >> 8};
    capture(EXTRA, bytes, sizeof(bytes));
}
_Static_assert(1 + NKRO_REPORT_BITS <= REPORT_BYTES, "NKRO capture size");
_Static_assert(1 + KEYBOARD_REPORT_KEYS <= REPORT_BYTES, "keyboard capture size");

// The matrix continues scanning/synchronizing, but physical edges cannot mix
// with synthetic events during capture. QMK's normal tick events still run.
bool should_process_keypress(void) { return is_keyboard_master() && !active; }

static void stop(void) {
    running = false;
    count = head = 0;
    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
            if (held[row] & ((matrix_row_t)1 << col)) action_exec(MAKE_KEYEVENT(row, col, false));
        }
    }
    memset(held, 0, sizeof(held));
    memset(projected, 0, sizeof(projected));
    clear_keyboard();
    // Stay captured until reboot: pending tap dances/macros must never leak
    // delayed reports to the desktop after an abort or transport failure.
    aborted = true;
}

void keytest_task(void) {
    if (!is_keyboard_master()) return;
    uint32_t now = timer_read32();
    if (reboot_pending && (int32_t)(now - reboot_at) >= 0) {
        if (reboot_bootloader) reset_keyboard(); else soft_reset_keyboard();
        return;
    }
    if (!active) return;
    if (!aborted && timer_elapsed32(last_command) >= WATCHDOG_MS) stop();
    if (!running || (int32_t)(now - deadline) < 0) return;
    test_event_t e = events[head];
    head = (head + 1) % EVENT_CAPACITY;
    --count;
    matrix_row_t mask = (matrix_row_t)1 << e.col;
    if (e.pressed) held[e.row] |= mask; else held[e.row] &= ~mask;
    uint8_t bytes[] = {e.row, e.col, e.pressed};
    capture(INPUT, bytes, sizeof(bytes));
    action_exec(MAKE_KEYEVENT(e.row, e.col, e.pressed));
    // Relative to actual execution, never catch up by shortening a hold.
    if (count) deadline = timer_read32() + events[head].delay; else running = false;
}

void keytest_command(uint8_t *data, uint8_t length) {
    // Need 3 header + 1 status + 22 result bytes. Never inspect wrapper tail.
    if (length < 26) { if (length > 3) data[3] = INVALID; return; }
    uint8_t args[RESPONSE_BYTES];
    memcpy(args, data + 3, sizeof(args));
    memset(data + 3, 0, 23);
    uint8_t *status = data + 3, *out = data + 4;
    if (data[0] != (data[2] == INFO ? id_custom_get_value : id_custom_set_value) || !is_keyboard_master()) { *status = INVALID; return; }
    last_command = timer_read32();
    switch (data[2]) {
        case INFO:
            out[0] = 1; // protocol
            out[1] = active | (running << 1) | (aborted << 2);
            out[2] = MATRIX_ROWS; out[3] = MATRIX_COLS;
            out[4] = EVENT_CAPACITY; out[5] = REPORT_CAPACITY; out[6] = count;
            put32(out + 7, first_report); put32(out + 11, next_report); put32(out + 15, lost_reports); out[19] = DYNAMIC_KEYMAP_LAYER_COUNT;
            break;
        case BEGIN: {
            if (memcmp(args, "TEST", 4)) { *status = INVALID; break; }
            if (active || !host_get_driver()) { *status = BUSY; break; }
            for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
                if (matrix_get_row(row)) { *status = BUSY; return; }
            }
            clear_keyboard(); // Release real host keys before installing capture.
            capture_driver = *host_get_driver(); // Preserve LEDs and raw HID transport.
            capture_driver.send_keyboard = capture_keyboard_report;
            capture_driver.send_nkro = capture_nkro_report;
            capture_driver.send_mouse = mouse_report;
            capture_driver.send_extra = extra_report;
            active = true;
            host_set_driver(&capture_driver);
            break;
        }
        case REBOOT:
            if (!active) { *status = INACTIVE; break; }
            if (args[0] > 1) { *status = INVALID; break; }
            stop();
            reboot_bootloader = args[0] == 1;
            reboot_pending = true; reboot_at = timer_read32() + 100; // Reply before disconnect.
            break;
        default:
            if (!active) { *status = INACTIVE; break; }
            if (data[2] == ABORT) { stop(); break; }
            if (data[2] == READ) {
                uint32_t seq = get32(args);
                uint8_t offset = args[4];
                if (seq < first_report || seq >= next_report) { *status = MISSING; break; }
                capture_t *r = &reports[seq % REPORT_CAPACITY];
                if (offset >= r->length) { *status = INVALID; break; }
                out[0] = r->kind; out[1] = r->length;
                put32(out + 2, r->time); put32(out + 6, seq); out[10] = offset;
                uint8_t n = r->length - offset;
                if (n > 11) n = 11;
                memcpy(out + 11, r->bytes + offset, n);
                break;
            }
            if (data[2] == ACK) {
                uint32_t seq = get32(args);
                if (seq < first_report || seq > next_report) { *status = INVALID; break; }
                first_report = seq;
                break;
            }
            if (data[2] == STATE) {
                put32(out, layer_state); put32(out + 4, default_layer_state);
                out[8] = get_mods(); out[9] = get_weak_mods();
                break;
            }
            if (aborted) { *status = ABORTED; break; }
            if (running) { *status = BUSY; break; }
            if (data[2] == SELECT_LAYER) {
                if (count) { *status = BUSY; break; }
                for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
                    if (held[row]) { *status = BUSY; return; }
                }
                if (args[0] >= DYNAMIC_KEYMAP_LAYER_COUNT || args[0] >= 32) { *status = INVALID; break; }
                layer_clear();
                default_layer_set((layer_state_t)1 << args[0]); // RAM only, restored by reboot.
            } else if (data[2] == ENQUEUE) {
                uint8_t row = args[2], col = args[3], pressed = args[4];
                if (row >= MATRIX_ROWS || col >= MATRIX_COLS || pressed > 1 || get16(args) > 60000) { *status = INVALID; break; }
                if (count == EVENT_CAPACITY) { *status = FULL; break; }
                matrix_row_t mask = (matrix_row_t)1 << col;
                if (!!(projected[row] & mask) == !!pressed) { *status = INVALID; break; }
                events[(head + count++) % EVENT_CAPACITY] = (test_event_t){get16(args), row, col, pressed};
                if (pressed) projected[row] |= mask; else projected[row] &= ~mask;
            } else if (data[2] == RUN) {
                if (!count) { *status = INVALID; break; }
                running = true; deadline = timer_read32() + events[head].delay;
            } else if (data[2] == CLEAR) {
                if (count) { *status = BUSY; break; }
                first_report = next_report = lost_reports = 0;
            } else *status = INVALID;
            break;
    }
}
