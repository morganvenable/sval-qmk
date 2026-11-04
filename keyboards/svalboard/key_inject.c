// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dev builds only (SVAL_TOUCH_DEV): key presses injected into a half's raw
// matrix, for timing tests with no hands on the keys. An injected key is OR-ed
// into the frame right after the rows are read, so it goes through debounce,
// the split link (for the half without USB), key processing and USB exactly as
// a real press. Each half runs its own schedule on its own timer.
//
// Raw HID 0xFC op 0x44 on the half with USB:
//   [7] half: 0 this one, 1 the other (over the split link)
//   [8] n events (1..4), then from [9] n x 6 bytes:
//       row (0..4, this hand's), col (0..5), start ms u16 (from receipt), hold ms u16
// Reply: [6] 0xC4, [7] status (0 ok, 1 the other half did not answer, 2 bad
// request), [8..11] this half's timer (ms) at receipt.
// A new command adds to the schedule (8 slots per half); an all-zero one
// (n 0) clears it.
#include "quantum.h"
#include "key_inject.h"
#include "transactions.h"
#include "split_util.h"
#include <string.h>

#define INJ_SLOTS 8
#define INJ_RPC 0xAE // Scan Lab RPC op (svalboard.c routes it here on the other half)

typedef struct {
    uint32_t start, end; // timer_read32 ms; end 0: free
    uint8_t  row, col;
} inj_t;

static inj_t inj[INJ_SLOTS];

static uint8_t inj_add(const uint8_t *ev, uint8_t n) {
    uint32_t now = timer_read32();
    if (n == 0) {
        memset(inj, 0, sizeof(inj));
        return 0;
    }
    if (n > 4) return 2;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *e = ev + 6 * i;
        if (e[0] >= 5 || e[1] >= MATRIX_COLS) return 2;
    }
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *e    = ev + 6 * i;
        uint16_t       from = e[2] | e[3] << 8, hold = e[4] | e[5] << 8;
        uint8_t        k;
        for (k = 0; k < INJ_SLOTS && inj[k].end; k++) {
        }
        if (k == INJ_SLOTS) return 2;
        inj[k] = (inj_t){now + from, now + from + (hold ? hold : 1), e[0], e[1]};
    }
    return 0;
}

void sval_key_inject_apply(matrix_row_t *rows) {
    uint32_t now = timer_read32();
    for (uint8_t k = 0; k < INJ_SLOTS; k++) {
        if (!inj[k].end) continue;
        if ((int32_t)(now - inj[k].end) >= 0) {
            inj[k].end = 0; // done: the key is up from this frame
            continue;
        }
        if ((int32_t)(now - inj[k].start) >= 0) rows[inj[k].row] |= (matrix_row_t)1 << inj[k].col;
    }
}

bool sval_key_inject_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 2 || in[0] != INJ_RPC || out_len < 6) return false;
    uint8_t  n   = in[1];
    uint32_t now = timer_read32();
    memset(out, 0, out_len);
    out[0] = INJ_RPC;
    out[1] = (in_len >= 2 + 6 * n) ? inj_add(in + 2, n) : 2;
    memcpy(out + 2, &now, 4);
    return true;
}


void sval_key_inject_command(uint8_t *data, uint8_t length) {
    uint8_t  half = data[7], n = data[8];
    uint32_t now  = timer_read32();
    uint8_t  status;
    if (n > 4 || 9 + 6 * n > length || half > 1) {
        status = 2;
    } else if (half == 0) {
        status = inj_add(data + 9, n);
    } else {
        uint8_t req[2 + 24] = {INJ_RPC, n}, reply[32] = {0};
        memcpy(req + 2, data + 9, 6 * n);
        if (!is_transport_connected() || !transaction_rpc_exec(KEYBOARD_SYNC_B, 2 + 6 * n, req, 32, reply) || reply[0] != INJ_RPC)
            status = 1;
        else
            status = reply[1];
    }
    memset(data + 6, 0, length - 6);
    data[6] = 0xC4;
    data[7] = status;
    memcpy(data + 8, &now, 4);
}
