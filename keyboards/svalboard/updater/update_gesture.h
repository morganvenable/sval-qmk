// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The confirmation chord (D4, D5): Index South + Middle South held for
// SVAL_UPDATE_CHORD_HOLD_MS on the half with USB. Detected from the scanned
// matrix, never from keycodes, so a host that injects key events cannot make it
// (R11). While the updater waits for it, and afterwards until each key is let
// go, the two keys are removed from the matrix QMK sees, so whatever they are
// mapped to is never typed.

#include <stdbool.h>
#include <stdint.h>

#ifdef SVAL_UPDATER_HOST_TEST
typedef uint8_t matrix_row_t;
#else
#    include "matrix.h"
#endif

// Start watching (entering CONFIRM_WAIT). A hold only counts once both keys
// have been seen up after this, so keys already held at ARM do not confirm.
void update_gesture_arm(void);

// Stop watching (leaving CONFIRM_WAIT). The keys stay swallowed until released.
void update_gesture_disarm(void);

// Whether the chord was made since the last arm, and when.
bool update_gesture_done(uint32_t *done_ms);

// From the matrix scan (kb/matrix.c), before the rows are read, with the
// timing chosen for this frame. While the chord is awaited the waits are
// raised to at least SVAL_UPDATE_CHORD_PREWAIT_US / POSTWAIT_US, so a timing
// the host chose (saved values, a Scan Lab sweep) cannot make the chord keys
// misread as held.
void update_gesture_timing(uint16_t *prewait_us, uint16_t *postwait_us);

// From the matrix scan (kb/matrix.c), on this half's freshly scanned rows
// (SVAL_UPDATE_CHORD_ROW_A/B are local row indices). Clears the chord keys
// while they are swallowed.
void update_gesture_scan(matrix_row_t *local_rows);
