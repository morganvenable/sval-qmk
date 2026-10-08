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
