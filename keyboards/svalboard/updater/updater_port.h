// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What updater.c and update_gesture.c need from the rest of the firmware. The
// firmware versions are at the end of updater.c; the host tests
// (tests/sval_updater) supply their own, with a simulated clock.

#include <stdbool.h>
#include <stdint.h>

uint32_t updater_port_now_ms(void);

// Whether the packet being handled came inside the client wrapper, and if so
// its client ID (never 0).
bool updater_port_client(uint32_t *client_id);

// QMK wear leveling has latched a failed settings write (qmk#12, D12).
bool updater_port_settings_failing(void);

// 32 bits for the session nonce. Not secret (D3); never 0.
uint32_t updater_port_random32(void);

// A Scan Lab sweep is running: it sets the matrix scan timing from the host,
// so ARM waits until it ends (the chord must come from a normally scanned
// matrix).
bool updater_port_scan_override(void);

// The other half as split presence shows it: update_presence_status_t (M2,
// V). Always UPDATE_PRESENCE_NONE on the half without USB.
uint8_t updater_port_split_presence(void);

// Main's stack (ChibiOS's process stack): its size, and the bytes at its
// bottom never used since boot (still crt0's fill pattern). M1 #13, R5.
uint16_t updater_port_stack_free(uint16_t *size);
