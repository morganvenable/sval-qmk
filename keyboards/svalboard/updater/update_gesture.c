// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The confirmation chord (see update_gesture.h).
//
// It reads the rows kb/matrix.c has just scanned, before debounce, because the
// keys must be removed before QMK's debounce and key processing see them. A
// bounce on either key restarts the hold, so the 1 s hold is itself stricter
// than debouncing.

#include "update_gesture.h"
#include "updater_port.h"

#define KEY_A 0x01
#define KEY_B 0x02
#define COL_BIT ((matrix_row_t)1 << SVAL_UPDATE_CHORD_COL)

static bool     watching; // CONFIRM_WAIT
static bool     seen_up;  // both keys were up at some scan since arm
static bool     holding;
static bool     done;
static uint32_t hold_ms;
static uint32_t done_ms;
static uint8_t  swallow; // KEY_A | KEY_B: removed from the matrix QMK sees

void update_gesture_arm(void) {
    watching = true;
    seen_up  = false;
    holding  = false;
    done     = false;
    swallow  = KEY_A | KEY_B;
}

void update_gesture_disarm(void) {
    watching = false;
    holding  = false;
}

bool update_gesture_done(uint32_t *when) {
    if (done && when) *when = done_ms;
    return done;
}

void update_gesture_timing(uint16_t *prewait_us, uint16_t *postwait_us) {
    if (!watching) return;
    if (*prewait_us < SVAL_UPDATE_CHORD_PREWAIT_US) *prewait_us = SVAL_UPDATE_CHORD_PREWAIT_US;
    if (*postwait_us < SVAL_UPDATE_CHORD_POSTWAIT_US) *postwait_us = SVAL_UPDATE_CHORD_POSTWAIT_US;
}

void update_gesture_scan(matrix_row_t *rows) {
    if (!watching && !swallow) return;
    bool a = rows[SVAL_UPDATE_CHORD_ROW_A] & COL_BIT;
    bool b = rows[SVAL_UPDATE_CHORD_ROW_B] & COL_BIT;

    if (watching) {
        swallow = KEY_A | KEY_B;
        if (!done) {
            if (!a && !b) seen_up = true;
            if (a && b && seen_up) {
                uint32_t now = updater_port_now_ms();
                if (!holding) {
                    holding = true;
                    hold_ms = now;
                } else if (now - hold_ms >= SVAL_UPDATE_CHORD_HOLD_MS) {
                    done    = true;
                    done_ms = now;
                }
            } else {
                holding = false;
            }
        }
    } else {
        // After the wait: each key comes back once it has been let go, so a
        // key still held when the wait ends never produces a press.
        if (!a) swallow &= ~KEY_A;
        if (!b) swallow &= ~KEY_B;
    }

    if (swallow & KEY_A) rows[SVAL_UPDATE_CHORD_ROW_A] &= ~COL_BIT;
    if (swallow & KEY_B) rows[SVAL_UPDATE_CHORD_ROW_B] &= ~COL_BIT;
}
