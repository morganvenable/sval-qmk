/*
Copyright 2026 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "mainboard.h"
#include <string.h>
#include "quantum.h"
#include "svalboard.h"
#include "two_cable.h"
#include "sval.h"
#ifdef SVAL_EITHER_HAND
#    include "side.h"
#endif

// Matrix helpers (matrix.c).
extern const pin_t col_pins[MATRIX_COLS];
bool               select_row(uint8_t row);
void               unselect_row(uint8_t row);

#ifndef SVAL_BOARD_ID_PIN
#    define SVAL_BOARD_ID_PIN GP3
#endif
#define BOARD_ID_SAMPLES 8

// Finger rows only (1-4). Measured on a Mini (sval:E46450D2A7094232, Scan Lab sweeps):
// a finger cluster made for the Mini reads correctly from 26 us (center; sides from
// 10 us), an original finger cluster lights at 145-155 us. The early read at 60 us
// sits 2.3x above the one and 2.4x below the other. The check times its own reads
// before the first scan, so the scan pre-wait (45 us on a Mini) doesn't move it. The
// thumb row is not checked.
#define CC_FIRST_ROW  1
#define CC_ROWS       5
#define CC_GAP_US     200 // all rows dark before each read (results flat from 100 us to 10 ms)
#define CC_EARLY_US   60
#define CC_LATE_US    400 // an original cluster is lit by ~155 us (longest seen)
#define CC_EARLY_MAX  100 // a late early read (interrupted) can't tell: vote unknown
#define CC_REPS       3
#define CC_OPTICAL    0x1F // cols 0-4; col 5 is the thumb Down tact switch (and unused on fingers)

static int8_t                board_cache = -1;
static sval_board_identity_t local_id;
static sval_board_identity_t other_id;
static bool                  other_valid = false;
static uint32_t              board_us, check_us; // time spent at boot (Sval 0x38 trailer)
static uint8_t               muted_rows;         // rows with an original cluster: keys muted

static inline uint32_t now_us(void) {
    return (uint32_t)chVTGetSystemTimeX(); // 1 MHz system timer (matrix.c asserts it)
}

// One sample: bit0 pull-down read, bit2 release read (bit1, the pull-up read, is
// taken once at the end, for diagnosis only: ~1 V on a Mini, so it decides nothing).
static uint8_t board_id_sample(void) {
    pin_t pin = SVAL_BOARD_ID_PIN;
    gpio_set_pin_input_low(pin);
    wait_us(20);
    if (gpio_read_pin(pin)) return 1 | 4; // driven high: V3.1 / V3.2, never driven against
    // Nothing drives the pin, so it is safe to drive it: a Mini sinks 110 uA
    // through R1, a V3.0 header nothing.
    ATOMIC_BLOCK_FORCEON {
        gpio_set_pin_output(pin);
        gpio_write_pin_high(pin);
        wait_us(5);
        gpio_set_pin_input(pin);
    }
    wait_us(20);
    bool rel = gpio_read_pin(pin);
    gpio_set_pin_input_low(pin);
    return rel ? 4 : 0;
}

// Only "pull-down low and release low" in every sample is a Mini. Anything
// else, including disagreement, is a normal board.
static uint8_t board_from_sample(uint8_t s) {
    if (s & 1) return SVAL_MAIN_BOARD_V3_1_V3_2;
    return (s & 4) ? SVAL_MAIN_BOARD_V3_0_V2B : SVAL_MAIN_BOARD_MINI_V1_0;
}

uint8_t sval_main_board(void) {
    if (board_cache >= 0) return (uint8_t)board_cache;
#if defined(SVAL_MAIN_BOARD_FORCE)
    // Dev-only: a prototype main board this rule can't see (never in a release build).
    board_cache  = SVAL_MAIN_BOARD_FORCE;
    local_id.gp3 = 0xF0;
#else
    uint32_t t0    = now_us();
    uint8_t  first = board_id_sample(), last = first, agree = 1;
    uint8_t  board = board_from_sample(first);
    for (uint8_t i = 1; i < BOARD_ID_SAMPLES; i++) {
        last = board_id_sample();
        if (board_from_sample(last) == board) agree++;
    }
    if (agree != BOARD_ID_SAMPLES) board = SVAL_MAIN_BOARD_NOT_IDENTIFIED; // regular timing, no check
    gpio_set_pin_input_high(SVAL_BOARD_ID_PIN);
    wait_us(20);
    bool pu = gpio_read_pin(SVAL_BOARD_ID_PIN);
    // Back to the RP2040's reset state for the pin (input, pull-down), as before this read.
    gpio_set_pin_input_low(SVAL_BOARD_ID_PIN);
    board_cache  = board;
    local_id.gp3 = (uint8_t)((last & 5) | (pu ? 2 : 0) | (agree << 4));
    board_us     = now_us() - t0;
#endif
    local_id.main_board = (uint8_t)board_cache;
    return (uint8_t)board_cache;
}

// One read of one row: SVAL_CLUSTER_FAST, _ORIGINAL, _DARK, or _NOT_CHECKED (interrupted).
static uint8_t cluster_read_row(uint8_t row) {
    wait_us(CC_GAP_US);
    uint8_t  early = 0, late = 0;
    uint32_t t0 = now_us();
    select_row(row);
    while ((uint32_t)(now_us() - t0) < CC_EARLY_US) {}
    for (uint8_t c = 0; c < MATRIX_COLS; c++) {
        if (!gpio_read_pin(col_pins[c])) early |= 1u << c; // lit = low
    }
    uint32_t t_early = now_us() - t0;
    early &= CC_OPTICAL;
    if (early) {
        unselect_row(row);
        return t_early <= CC_EARLY_MAX ? SVAL_CLUSTER_FAST : SVAL_CLUSTER_NOT_CHECKED;
    }
    while ((uint32_t)(now_us() - t0) < CC_LATE_US) {}
    for (uint8_t c = 0; c < MATRIX_COLS; c++) {
        if (!gpio_read_pin(col_pins[c])) late |= 1u << c;
    }
    unselect_row(row);
    if (t_early > CC_EARLY_MAX) return SVAL_CLUSTER_NOT_CHECKED;
    return (late & CC_OPTICAL) ? SVAL_CLUSTER_ORIGINAL : SVAL_CLUSTER_DARK;
}

void sval_cluster_check(void) {
    if (sval_main_board() != SVAL_MAIN_BOARD_MINI_V1_0) return;
    uint32_t t0 = now_us();
    for (uint8_t r = 0; r < CC_ROWS; r++) unselect_row(r);
    uint16_t classes = 0;
    bool     unsupported = false;
    for (uint8_t r = CC_FIRST_ROW; r < CC_ROWS; r++) {
        uint8_t cls = cluster_read_row(r);
        if (cls == SVAL_CLUSTER_ORIGINAL || cls == SVAL_CLUSTER_NOT_CHECKED) {
            // Fast and dark are decisive after one read (only a fast emitter is lit at
            // 60 us, and dark is never reported). The one result that is reported,
            // "original", needs two of three reads to agree.
            uint8_t votes[4] = {0};
            votes[cls]++;
            for (uint8_t i = 1; i < CC_REPS; i++) votes[cluster_read_row(r)]++;
            cls = SVAL_CLUSTER_NOT_CHECKED;
            if (votes[SVAL_CLUSTER_FAST]) cls = SVAL_CLUSTER_FAST;
            else if (votes[SVAL_CLUSTER_ORIGINAL] >= 2) cls = SVAL_CLUSTER_ORIGINAL;
            else if (votes[SVAL_CLUSTER_DARK] >= 2) cls = SVAL_CLUSTER_DARK;
        }
        if (cls == SVAL_CLUSTER_ORIGINAL) {
            unsupported = true;
            muted_rows |= (uint8_t)(1u << r);
        }
        classes |= (uint16_t)cls << (2 * r);
    }
    local_id.clusters = classes;
    check_us          = now_us() - t0;
    local_id.flags |= SVAL_BOARD_CHECK_RAN | (unsupported ? SVAL_BOARD_UNSUPPORTED : 0);
}

// An original cluster on a Mini can't be scanned: its center key would read as held
// all the time. Its row's keys are muted instead (matrix.c); Scan Lab still sees them.
uint8_t sval_muted_rows(void) {
    return muted_rows;
}

void sval_local_board_identity(sval_board_identity_t *id) {
    sval_main_board();
    *id = local_id;
    id->flags &= (uint8_t)(SVAL_BOARD_CHECK_RAN | SVAL_BOARD_UNSUPPORTED);
    id->flags |= SVAL_BOARD_REPORTED | (is_keyboard_left() ? SVAL_BOARD_LEFT : 0);
#ifdef SVAL_EITHER_HAND
    if (!sval_side_known()) id->flags = (uint8_t)((id->flags & ~SVAL_BOARD_LEFT) | SVAL_BOARD_SIDE_UNKNOWN);
#endif
}

void sval_other_board_set(const sval_board_identity_t *id) {
    other_valid = id != NULL;
    if (id) {
        other_id = *id;
    } else {
        memset(&other_id, 0, sizeof(other_id));
    }
}

// Sval MAIN_BOARD (0x38) reply, after [0xDF] [0x38]:
// [format = 1] [this half: 5 bytes] [other half: 5 bytes], each a sval_board_identity_t,
// then this half's boot cost: [board read us u16] [cluster check us u16] (diagnostic).
// The other half's block stays zero while it is disconnected or runs firmware without this.
void sval_main_board_kb(uint8_t *out, uint8_t length) {
    if (length < 1 + 2 * sizeof(sval_board_identity_t)) return;
    sval_board_identity_t mine;
    sval_local_board_identity(&mine);
    out[0] = 1;
    memcpy(&out[1], &mine, sizeof(mine));
    sval_board_identity_t peer;
    if (other_valid && sval_other_half_connected())
        memcpy(&out[1 + sizeof(mine)], &other_id, sizeof(other_id));
    else if (sval_two_cable_peer_board(&peer)) // a two-cable lead: the follower's, through the relay
        memcpy(&out[1 + sizeof(mine)], &peer, sizeof(peer));
    if (length >= 15) {
        uint16_t b = board_us > 0xFFFF ? 0xFFFF : (uint16_t)board_us, c = check_us > 0xFFFF ? 0xFFFF : (uint16_t)check_us;
        out[11] = b & 0xFF;
        out[12] = b >> 8;
        out[13] = c & 0xFF;
        out[14] = c >> 8;
    }
}
