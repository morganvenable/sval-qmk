// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Svalboard debounce (DEBOUNCE_TYPE = custom, rules.mk).
//
// Every key but one is an optical interrupter, read once per frame while its
// row is lit. There is no contact to bounce, so those keys pass straight
// through: a raw change reaches the debounced matrix on the scan that sees it.
//
// The exception is the thumb Double Down key, a mechanical tact switch: local
// row 0 (the thumb row) in column DOUBLEDOWN_COL. The finger rows use the same
// column for their optical Double South keys, which pass through like the
// others. Double Down alone is debounced, per key and deferred: a change is
// reported once its raw level has held for SVAL_DD_DEBOUNCE_MS, and any raw
// change restarts the wait. Its bounce never delays another key.
//
// Each half debounces its own rows (matrix_scan() passes matrix + thisHand),
// so the one local position covers both thumbs: left [0,5] and right [5,5].

#ifdef SVAL_DEBOUNCE_HOST_TEST
#    include "debounce_sval_host.h" // tests/sval_debounce
#else
#    include "debounce.h"
#    include "timer.h"
#endif

#ifndef SVAL_DD_DEBOUNCE_MS
#    define SVAL_DD_DEBOUNCE_MS 5
#endif
#if SVAL_DD_DEBOUNCE_MS < 1 || SVAL_DD_DEBOUNCE_MS > 250
#    error "SVAL_DD_DEBOUNCE_MS must be 1 to 250"
#endif

#define SVAL_DD_ROW 0 // the thumb row, local to each half
#define SVAL_DD_BIT ((matrix_row_t)1 << DOUBLEDOWN_COL)

static bool     dd_waiting; // raw Double Down differs from debounced and is being timed
static uint16_t dd_since;   // when raw last moved away from debounced, in ms

void debounce_init(void) {
    dd_waiting = false;
    dd_since   = 0;
}

// One pass at time now_ms. Split out of debounce() for the host tests.
bool sval_debounce_step(matrix_row_t raw[], matrix_row_t cooked[], uint16_t now_ms) {
    bool changed = false;

    // Optical keys: straight through. Double Down keeps its debounced bit.
    for (uint8_t r = 0; r < MATRIX_ROWS_PER_HAND; r++) {
        matrix_row_t keep = (r == SVAL_DD_ROW) ? SVAL_DD_BIT : 0;
        matrix_row_t next = (raw[r] & (matrix_row_t)~keep) | (cooked[r] & keep);
        if (next != cooked[r]) {
            cooked[r] = next;
            changed   = true;
        }
    }

    // Double Down: deferred, per key.
    bool raw_dd    = (raw[SVAL_DD_ROW] & SVAL_DD_BIT) != 0;
    bool cooked_dd = (cooked[SVAL_DD_ROW] & SVAL_DD_BIT) != 0;
    if (raw_dd == cooked_dd) {
        dd_waiting = false; // bounced back: the next change starts a new wait
    } else {
        if (!dd_waiting) {
            dd_waiting = true;
            dd_since   = now_ms;
        }
        if ((uint16_t)(now_ms - dd_since) >= SVAL_DD_DEBOUNCE_MS) {
            cooked[SVAL_DD_ROW] ^= SVAL_DD_BIT;
            dd_waiting = false;
            changed    = true;
        }
    }
    return changed;
}

// matrix_scan() calls this on every pass, changed or not, so the Double Down
// wait also ends on passes without a new frame.
bool debounce(matrix_row_t raw[], matrix_row_t cooked[], bool changed) {
    (void)changed;
#ifdef SVAL_DEBOUNCE_HOST_TEST
    return sval_debounce_step(raw, cooked, host_now_ms());
#else
    return sval_debounce_step(raw, cooked, timer_read());
#endif
}
