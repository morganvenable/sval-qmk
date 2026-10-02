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
#pragma once

/*
 * Scan Lab: in-firmware characterization of the optical key matrix.
 *
 * Two instruments, both driven from the host over the VIA custom-value
 * protocol on SCANLAB_CHANNEL and relayed to the other half over split RPC:
 *
 *  Settle probe  Drive one row, then sample all sense lines in a tight loop
 *                with microsecond timestamps. For every column we record when
 *                the line last changed after the row was enabled ("settle"),
 *                how many times it toggled, and its final level; then the same
 *                after the row is released ("recovery"). With nothing pressed
 *                the lit level also tells you each key's polarity: a line that
 *                changes when its row lights is an active-dark key.
 *
 *  Sweep         Capture a reference frame at a safe timing, then run N frames
 *                at a candidate pre/post-wait and count per-key mismatches
 *                against the reference. Key events are suppressed while a
 *                sweep runs. Keys held during the sweep are part of the
 *                reference, so pressed states can be covered too.
 *
 * See keyboards/svalboard/docs/scan-lab.md for the wire format.
 */

#include "quantum.h"

#define SCANLAB_CHANNEL          0x53  // 'S': VIA custom-value channel
#define SCANLAB_PROTO_VERSION    1
#define SCANLAB_ROWS             5     // rows per hand
#define SCANLAB_COLS             MATRIX_COLS
#define SCANLAB_SAFE_PREWAIT_US  500
#define SCANLAB_SAFE_POSTWAIT_US 500
#define SCANLAB_PROBE_WINDOW_US  1500  // per phase (row on, row off)
#define SCANLAB_REF_FRAMES       8     // identical frames needed for a reference
#define SCANLAB_REF_MAX_TRIES    64    // frames to find one before giving up
#define SCANLAB_REBOOT_WINDOW_MS 5000  // "go" must follow "arm" within this
#define SCANLAB_REQ_LEN          24    // bytes: [op][args...]
#define SCANLAB_RSP_LEN          24    // bytes written back into the VIA packet (23 used)

// Request ops. SET ops arrive as VIA id_custom_set_value with value_id = op and
// value_data = [hand][args...]. GET ops arrive as id_custom_get_value with
// value_id = op | (hand << 3) | row. Hand: 0 = left, 1 = right.
enum scanlab_op {
    SCANLAB_OP_SET_MODE  = 0x01, // args: mode(1: 0 off, 1 sweep), prewait u16, postwait u16, frames u16
    SCANLAB_OP_PROBE     = 0x02, // args: row
    SCANLAB_OP_ABORT     = 0x03,
    SCANLAB_OP_REBOOT_ARM = 0x04, // set: returns a one-time token (needs SVAL_HOST_BOOTLOADER)
    SCANLAB_OP_REBOOT_GO  = 0x05, // set: args token u16; acks, then reboots into the bootloader
    SCANLAB_OP_STATUS    = 0x10, // get
    SCANLAB_OP_POWER     = 0x11, // get: pacing settings and measured frame/LED-on times
    SCANLAB_OP_SWEEP_ROW = 0x20, // get | hand<<3 | row
    SCANLAB_OP_PROBE_ON  = 0x40, // get | hand<<3 | row
    SCANLAB_OP_PROBE_OFF = 0x60, // get | hand<<3 | row
};

enum scanlab_state {
    SCANLAB_IDLE     = 0,
    SCANLAB_REF      = 1, // capturing reference at safe timing
    SCANLAB_RUN      = 2, // counting mismatches at test timing
    SCANLAB_DONE     = 3,
    SCANLAB_REF_FAIL = 4, // could not get SCANLAB_REF_FRAMES identical frames
};

void scanlab_init(void);
bool scanlab_active(void);                              // sweep in progress: suppress key events
bool scanlab_timing(uint16_t *pre, uint16_t *post);     // true when the engine overrides scan timing
void scanlab_on_frame(const matrix_row_t *raw);         // feed every raw (pre-debounce) frame
void scanlab_probe_row(uint8_t row);                    // run the settle probe on this hand
void scanlab_handle(const uint8_t *req, uint8_t *rsp);  // execute one request on this hand
void scanlab_via_command(uint8_t *data, uint8_t length); // master: VIA entry, routes to a hand
void scanlab_housekeeping(void);                        // call from housekeeping_task_kb on both halves
bool scanlab_reboot_armed(void);
